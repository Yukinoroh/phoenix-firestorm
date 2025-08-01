/**
 * @file llmeshrepository.cpp
 * @brief Mesh repository implementation.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010-2014, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llapr.h"
#include "apr_portable.h"
#include "apr_pools.h"
#include "apr_dso.h"
#include "llhttpconstants.h"
#include "llmeshrepository.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llbufferstream.h"
#include "llcallbacklist.h"
#include "lldatapacker.h"
#include "lldeadmantimer.h"
#include "llfloatermodelpreview.h"
#include "llfloaterperms.h"
#include "llimagej2c.h"
#include "llhost.h"
#include "llmath.h"
#include "llnotificationsutil.h"
#include "llsd.h"
#include "llsdutil_math.h"
#include "llsdserialize.h"
#include "llthread.h"
#include "llfilesystem.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewermenufile.h"
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerstatsrecorder.h"
#include "llviewertexturelist.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"
#include "llworld.h"
#include "material_codes.h"
#include "pipeline.h"
#include "llinventorymodel.h"
#include "llfoldertype.h"
#include "llviewerparcelmgr.h"
#include "lluploadfloaterobservers.h"
#include "bufferarray.h"
#include "bufferstream.h"
#include "llfasttimer.h"
#include "llcorehttputil.h"
#include "lltrans.h"
#include "llstatusbar.h"
#include "llinventorypanel.h"
#include "lluploaddialog.h"
#include "llfloaterreg.h"
#include "llvoavatarself.h"
#include "llskinningutil.h"

#include "boost/iostreams/device/array.hpp"
#include "boost/iostreams/stream.hpp"
#include "boost/lexical_cast.hpp"
// <FS:Beq pp Rye> Reduce temporaries and copes in decoding mesh headers
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
// </FS:Beq pp Rye>

#ifndef LL_WINDOWS
#include "netdb.h"
#endif

#include "llviewernetwork.h"

// Purpose
//
//   The purpose of this module is to provide access between the viewer
//   and the asset system as regards to mesh objects.
//
//   * High-throughput download of mesh assets from servers while
//     following best industry practices for network profile.
//   * Reliable expensing and upload of new mesh assets.
//   * Recovery and retry from errors when appropriate.
//   * Decomposition of mesh assets for preview and uploads.
//   * And most important:  all of the above without exposing the
//     main thread to stalls due to deep processing or thread
//     locking actions.  In particular, the following operations
//     on LLMeshRepository are very averse to any stalls:
//     * loadMesh
//     * search in mMeshHeader (For structural details, see:
//       http://wiki.secondlife.com/wiki/Mesh/Mesh_Asset_Format)
//     * notifyLoadedMeshes
//     * getSkinInfo
//
// Threads
//
//   main     Main rendering thread, very sensitive to locking and other stalls
//   repo     Overseeing worker thread associated with the LLMeshRepoThread class
//   decom    Worker thread for mesh decomposition requests
//   core     HTTP worker thread:  does the work but doesn't intrude here
//   uploadN  0-N temporary mesh upload threads (0-1 in practice)
//
// Sequence of Operations
//
//   What follows is a description of the retrieval of one LOD for
//   a new mesh object.  Work is performed by a series of short, quick
//   actions distributed over a number of threads.  Each is meant
//   to proceed without stalling and the whole forms a deep request
//   pipeline to achieve throughput.  Ellipsis indicates a return
//   or break in processing which is resumed elsewhere.
//
//         main thread         repo thread (run() method)
//
//         loadMesh() invoked to request LOD
//           append LODRequest to mPendingRequests
//         ...
//         other mesh requests may be made
//         ...
//         notifyLoadedMeshes() invoked to stage work
//           append HeaderRequest to mHeaderReqQ
//         ...
//                             scan mHeaderReqQ
//                             issue 4096-byte GET for header
//                             ...
//                             onCompleted() invoked for GET
//                               data copied
//                               headerReceived() invoked
//                                 LLSD parsed
//                                 mMeshHeader updated
//                                 scan mPendingLOD for LOD request
//                                 push LODRequest to mLODReqQ
//                             ...
//                             scan mLODReqQ
//                             fetchMeshLOD() invoked
//                               issue Byte-Range GET for LOD
//                             ...
//                             onCompleted() invoked for GET
//                               data copied
//                               lodReceived() invoked
//                                 unpack data into LLVolume
//                                 append LoadedMesh to mLoadedQ
//                             ...
//         notifyLoadedMeshes() invoked again
//           scan mLoadedQ
//           notifyMeshLoaded() for LOD
//             setMeshAssetLoaded() invoked for system volume
//             notifyMeshLoaded() invoked for each interested object
//         ...
//
// Mutexes
//
//   LLMeshRepository::mMeshMutex
//   LLMeshRepoThread::mMutex
//   LLMeshRepoThread::mHeaderMutex
//   LLMeshRepoThread::mSignal (LLCondition)
//   LLPhysicsDecomp::mSignal (LLCondition)
//   LLPhysicsDecomp::mMutex
//   LLMeshUploadThread::mMutex
//
// Mutex Order Rules
//
//   1.  LLMeshRepoThread::mMutex before LLMeshRepoThread::mHeaderMutex
//   2.  LLMeshRepository::mMeshMutex before LLMeshRepoThread::mMutex
//   (There are more rules, haven't been extracted.)
//
// Data Member Access/Locking
//
//   Description of how shared access to static and instance data
//   members is performed.  Each member is followed by the name of
//   the mutex, if any, covering the data and then a list of data
//   access models each of which is a triplet of the following form:
//
//     {ro, wo, rw}.{main, repo, any}.{mutex, none}
//     Type of access:  read-only, write-only, read-write.
//     Accessing thread or 'any'
//     Relevant mutex held during access (several may be held) or 'none'
//
//   A careful eye will notice some unsafe operations.  Many of these
//   have an alibi of some form.  Several types of alibi are identified
//   and listed here:
//
//     [0]  No alibi.  Probably unsafe.
//     [1]  Single-writer, self-consistent readers.  Old data must
//          be tolerated by any reader but data will come true eventually.
//     [2]  Like [1] but provides a hint about thread state.  These
//          may be unsafe.
//     [3]  empty() check outside of lock.  Can me made safish when
//          done in double-check lock style.  But this depends on
//          std:: implementation and memory model.
//     [4]  Appears to be covered by a mutex but doesn't need one.
//     [5]  Read of a double-checked lock.
//
//   So, in addition to documentation, take this as a to-do/review
//   list and see if you can improve things.  For porters to non-x86
//   architectures, the weaker memory models will make these platforms
//   probabilistically more susceptible to hitting race conditions.
//   True here and in other multi-thread code such as texture fetching.
//   (Strong memory models make weak programmers.  Weak memory models
//   make strong programmers.  Ref:  arm, ppc, mips, alpha)
//
//   LLMeshRepository:
//
//     sBytesReceived                  none            rw.repo.none, ro.main.none [1]
//     sMeshRequestCount               "
//     sHTTPRequestCount               "
//     sHTTPLargeRequestCount          "
//     sHTTPRetryCount                 "
//     sHTTPErrorCount                 "
//     sLODPending                     mMeshMutex [4]  rw.main.mMeshMutex
//     sLODProcessing                  Repo::mMutex    rw.any.Repo::mMutex
//     sCacheBytesRead                 none            rw.repo.none, ro.main.none [1]
//     sCacheBytesWritten              "
//     sCacheReads                     "
//     sCacheWrites                    "
//     mLoadingMeshes                  mMeshMutex [4]  rw.main.none, rw.any.mMeshMutex
//     mSkinMap                        none            rw.main.none
//     mDecompositionMap               none            rw.main.none
//     mPendingRequests                mMeshMutex [4]  rw.main.mMeshMutex
//     mLoadingSkins                   mMeshMutex [4]  rw.main.mMeshMutex
//     mPendingSkinRequests            mMeshMutex [4]  rw.main.mMeshMutex
//     mLoadingDecompositions          mMeshMutex [4]  rw.main.mMeshMutex
//     mPendingDecompositionRequests   mMeshMutex [4]  rw.main.mMeshMutex
//     mLoadingPhysicsShapes           mMeshMutex [4]  rw.main.mMeshMutex
//     mPendingPhysicsShapeRequests    mMeshMutex [4]  rw.main.mMeshMutex
//     mUploads                        none            rw.main.none (upload thread accessing objects)
//     mUploadWaitList                 none            rw.main.none (upload thread accessing objects)
//     mInventoryQ                     mMeshMutex [4]  rw.main.mMeshMutex, ro.main.none [5]
//     mUploadErrorQ                   mMeshMutex      rw.main.mMeshMutex, rw.any.mMeshMutex
//     mGetMeshVersion                 none            rw.main.none
//
//   LLMeshRepoThread:
//
//     sActiveHeaderRequests    mMutex        rw.any.mMutex, ro.repo.none [1]
//     sActiveLODRequests       mMutex        rw.any.mMutex, ro.repo.none [1]
//     sMaxConcurrentRequests   mMutex        wo.main.none, ro.repo.none, ro.main.mMutex
//     mMeshHeader              mHeaderMutex  rw.repo.mHeaderMutex, ro.main.mHeaderMutex, ro.main.none [0]
//     mSkinRequests            mMutex        rw.repo.mMutex, ro.repo.none [5]
//     mSkinInfoQ               mMutex        rw.repo.mMutex, rw.main.mMutex [5] (was:  [0])
//     mDecompositionRequests   mMutex        rw.repo.mMutex, ro.repo.none [5]
//     mPhysicsShapeRequests    mMutex        rw.repo.mMutex, ro.repo.none [5]
//     mDecompositionQ          mMutex        rw.repo.mLoadedMutex, rw.main.mLoadedMutex [5] (was:  [0])
//     mHeaderReqQ              mMutex        ro.repo.none [5], rw.repo.mMutex, rw.any.mMutex
//     mLODReqQ                 mMutex        ro.repo.none [5], rw.repo.mMutex, rw.any.mMutex
//     mUnavailableQ            mMutex        rw.repo.none [0], ro.main.none [5], rw.main.mLoadedMutex
//     mLoadedQ                 mMutex        rw.repo.mLoadedMutex, ro.main.none [5], rw.main.mLoadedMutex
//     mPendingLOD              mMutex        rw.repo.mPendingMutex, rw.any.mPendingMutex
//     mGetMeshCapability       mMutex        rw.main.mMutex, ro.repo.mMutex (was:  [0])
//     mGetMesh2Capability      mMutex        rw.main.mMutex, ro.repo.mMutex (was:  [0])
//     mGetMeshVersion          mMutex        rw.main.mMutex, ro.repo.mMutex
//     mHttp*                   none          rw.repo.none
//
//   LLMeshUploadThread:
//
//     mDiscarded               mMutex        rw.main.mMutex, ro.uploadN.none [1]
//     ... more ...
//
// QA/Development Testing
//
//   Debug variable 'MeshUploadFakeErrors' takes a mask of bits that will
//   simulate an error on fee query or upload.  Defined bits are:
//
//   0x01            Simulate application error on fee check reading
//                   response body from file "fake_upload_error.xml"
//   0x02            Same as 0x01 but for actual upload attempt.
//   0x04            Simulate a transport problem on fee check with a
//                   locally-generated 500 status.
//   0x08            As with 0x04 but for the upload operation.
//
//   For major changes, see the LL_MESH_FASTTIMER_ENABLE below and
//   instructions for looking for frame stalls using fast timers.
//
// *TODO:  Work list for followup actions:
//   * Review anything marked as unsafe above, verify if there are real issues.
//   * See if we can put ::run() into a hard sleep.  May not actually perform better
//     than the current scheme so be prepared for disappointment.  You'll likely
//     need to introduce a condition variable class that references a mutex in
//     methods rather than derives from mutex which isn't correct.
//   * On upload failures, make more information available to the alerting
//     dialog.  Get the structured information going into the log into a
//     tree there.
//   * Header parse failures come without much explanation.  Elaborate.
//   * Work queue for uploads?  Any need for this or is the current scheme good
//     enough?
//   * Move data structures holding mesh data used by main thread into main-
//     thread-only access so that no locking is needed.  May require duplication
//     of some data so that worker thread has a minimal data set to guide
//     operations.
//
// --------------------------------------------------------------------------
//                    Development/Debug/QA Tools
//
// Enable here or in build environment to get fasttimer data on mesh fetches.
//
// Typically, this is used to perform A/B testing using the
// fasttimer console (shift-ctrl-9).  This is done by looking
// for stalls due to lock contention between the main thread
// and the repository and HTTP code.  In a release viewer,
// these appear as ping-time or worse spikes in frame time.
// With this instrumentation enabled, a stall will appear
// under the 'Mesh Fetch' timer which will be either top-level
// or under 'Render' time.

static LLFastTimer::DeclareTimer FTM_MESH_FETCH("Mesh Fetch");

// Random failure testing for development/QA.
//
// Set the MESH_*_FAILED macros to either 'false' or to
// an invocation of MESH_RANDOM_NTH_TRUE() with some
// suitable number.  In production, all must be false.
//
// Example:
// #define  MESH_HTTP_RESPONSE_FAILED               MESH_RANDOM_NTH_TRUE(9)

// 1-in-N calls will test true
#define MESH_RANDOM_NTH_TRUE(_N)                ( ll_rand(S32(_N)) == 0 )

#define MESH_HTTP_RESPONSE_FAILED               false
#define MESH_HEADER_PROCESS_FAILED              false
#define MESH_LOD_PROCESS_FAILED                 false
#define MESH_SKIN_INFO_PROCESS_FAILED           false
#define MESH_DECOMP_PROCESS_FAILED              false
#define MESH_PHYS_SHAPE_PROCESS_FAILED          false

// --------------------------------------------------------------------------


LLMeshRepository gMeshRepo;

constexpr U32 CACHE_PREAMBLE_VERSION = 1;
constexpr S32 CACHE_PREAMBLE_SIZE = sizeof(U32) * 3; //version, header_size, flags
constexpr S32 MESH_HEADER_SIZE = 4096;                      // Important:  assumption is that headers fit in this space

// <FS:Ansariel> [UDP Assets]
const S32 REQUEST_HIGH_WATER_MIN = 32;                  // Limits for GetMesh regions
const S32 REQUEST_HIGH_WATER_MAX = 150;                 // Should remain under 2X throttle
const S32 REQUEST_LOW_WATER_MIN = 16;
const S32 REQUEST_LOW_WATER_MAX = 75;
// </FS:Ansariel> [UDP Assets]

constexpr S32 REQUEST2_HIGH_WATER_MIN = 32;                 // Limits for GetMesh2 regions
constexpr S32 REQUEST2_HIGH_WATER_MAX = 100;
constexpr S32 REQUEST2_LOW_WATER_MIN = 16;
constexpr S32 REQUEST2_LOW_WATER_MAX = 50;

constexpr U32 LARGE_MESH_FETCH_THRESHOLD = 1U << 21;        // Size at which requests goes to narrow/slow queue
constexpr long SMALL_MESH_XFER_TIMEOUT = 120L;              // Seconds to complete xfer, small mesh downloads
constexpr long LARGE_MESH_XFER_TIMEOUT = 600L;              // Seconds to complete xfer, large downloads

constexpr U32 DOWNLOAD_RETRY_LIMIT = 8;
constexpr F32 DOWNLOAD_RETRY_DELAY = 0.5f; // seconds

// Would normally like to retry on uploads as some
// retryable failures would be recoverable.  Unfortunately,
// the mesh service is using 500 (retryable) rather than
// 400/bad request (permanent) for a bad payload and
// retrying that just leads to revocation of the one-shot
// cap which then produces a 404 on retry destroying some
// (occasionally) useful error information.  We'll leave
// upload retries to the user as in the past.  SH-4667.
constexpr long UPLOAD_RETRY_LIMIT = 0L;

// Maximum mesh version to support.  Three least significant digits are reserved for the minor version,
// with major version changes indicating a format change that is not backwards compatible and should not
// be parsed by viewers that don't specifically support that version. For example, if the integer "1" is
// present, the version is 0.001. A viewer that can parse version 0.001 can also parse versions up to 0.999,
// but not 1.0 (integer 1000).
// See wiki at https://wiki.secondlife.com/wiki/Mesh/Mesh_Asset_Format
constexpr S32 MAX_MESH_VERSION = 999;

//<FS:TS> FIRE-11451: Cap concurrent mesh requests at a sane value
const U32 MESH_CONCURRENT_REQUEST_LIMIT = 64;  // upper limit
const U32 MESH2_CONCURRENT_REQUEST_LIMIT = 32;  // upper limit
//</FS:TS> FIRE-11451

U32 LLMeshRepository::sBytesReceived = 0;
U32 LLMeshRepository::sMeshRequestCount = 0;
U32 LLMeshRepository::sHTTPRequestCount = 0;
U32 LLMeshRepository::sHTTPLargeRequestCount = 0;
U32 LLMeshRepository::sHTTPRetryCount = 0;
U32 LLMeshRepository::sHTTPErrorCount = 0;
U32 LLMeshRepository::sLODProcessing = 0;
U32 LLMeshRepository::sLODPending = 0;

U32 LLMeshRepository::sCacheBytesRead = 0;
std::atomic<U32> LLMeshRepository::sCacheBytesWritten = 0;
U32 LLMeshRepository::sCacheBytesHeaders = 0;
U32 LLMeshRepository::sCacheBytesSkins = 0;
U32 LLMeshRepository::sCacheBytesDecomps = 0;
U32 LLMeshRepository::sCacheReads = 0;
std::atomic<U32> LLMeshRepository::sCacheWrites = 0;
U32 LLMeshRepository::sMaxLockHoldoffs = 0;

LLDeadmanTimer LLMeshRepository::sQuiescentTimer(15.0, false);  // true -> gather cpu metrics

namespace {
    // The NoOpDeletor is used when passing certain objects (generally the LLMeshUploadThread)
    // in a smart pointer below for passage into the LLCore::Http libararies.
    // When the smart pointer is destroyed,  no action will be taken since we
    // do not in these cases want the object to be destroyed at the end of the call.
    //
    // *NOTE$: Yes! It is "Deletor"
    // http://english.stackexchange.com/questions/4733/what-s-the-rule-for-adding-er-vs-or-when-nouning-a-verb
    // "delete" derives from Latin "deletus"

    void NoOpDeletor(LLCore::HttpHandler *)
    { /*NoOp*/ }
}

static S32 dump_num = 0;
std::string make_dump_name(std::string prefix, S32 num)
{
    return prefix + std::to_string(num) + std::string(".xml");
}
void dump_llsd_to_file(const LLSD& content, std::string filename);
LLSD llsd_from_file(std::string filename);

const std::string header_lod[] =
{
    "lowest_lod",
    "low_lod",
    "medium_lod",
    "high_lod"
};
const char * const LOG_MESH = "Mesh";

// Static data and functions to measure mesh load
// time metrics for a new region scene.
static unsigned int metrics_teleport_start_count = 0;
boost::signals2::connection metrics_teleport_started_signal;
static void teleport_started();

void on_new_single_inventory_upload_complete(
    LLAssetType::EType asset_type,
    LLInventoryType::EType inventory_type,
    const std::string inventory_type_string,
    const LLUUID& item_folder_id,
    const std::string& item_name,
    const std::string& item_description,
    const LLSD& server_response,
    S32 upload_price);


//get the number of bytes resident in memory for given volume
U32 get_volume_memory_size(const LLVolume* volume)
{
    U32 indices = 0;
    U32 vertices = 0;

    for (S32 i = 0; i < volume->getNumVolumeFaces(); ++i)
    {
        const LLVolumeFace& face = volume->getVolumeFace(i);
        indices += face.mNumIndices;
        vertices += face.mNumVertices;
    }


    return indices*2+vertices*11+sizeof(LLVolume)+sizeof(LLVolumeFace)*volume->getNumVolumeFaces();
}

void get_vertex_buffer_from_mesh(LLCDMeshData& mesh, LLModel::PhysicsMesh& res, F32 scale = 1.f)
{
    res.mPositions.clear();
    res.mNormals.clear();

    const F32* v = mesh.mVertexBase;

    if (mesh.mIndexType == LLCDMeshData::INT_16)
    {
        U16* idx = (U16*) mesh.mIndexBase;
        for (S32 j = 0; j < mesh.mNumTriangles; ++j)
        {
            F32* mp0 = (F32*) ((U8*)v+idx[0]*mesh.mVertexStrideBytes);
            F32* mp1 = (F32*) ((U8*)v+idx[1]*mesh.mVertexStrideBytes);
            F32* mp2 = (F32*) ((U8*)v+idx[2]*mesh.mVertexStrideBytes);

            idx = (U16*) (((U8*)idx)+mesh.mIndexStrideBytes);

            LLVector3 v0(mp0);
            LLVector3 v1(mp1);
            LLVector3 v2(mp2);

            LLVector3 n = (v1-v0)%(v2-v0);
            n.normalize();

            res.mPositions.push_back(v0*scale);
            res.mPositions.push_back(v1*scale);
            res.mPositions.push_back(v2*scale);

            res.mNormals.push_back(n);
            res.mNormals.push_back(n);
            res.mNormals.push_back(n);
        }
    }
    else
    {
        U32* idx = (U32*) mesh.mIndexBase;
        for (S32 j = 0; j < mesh.mNumTriangles; ++j)
        {
            F32* mp0 = (F32*) ((U8*)v+idx[0]*mesh.mVertexStrideBytes);
            F32* mp1 = (F32*) ((U8*)v+idx[1]*mesh.mVertexStrideBytes);
            F32* mp2 = (F32*) ((U8*)v+idx[2]*mesh.mVertexStrideBytes);

            idx = (U32*) (((U8*)idx)+mesh.mIndexStrideBytes);

            LLVector3 v0(mp0);
            LLVector3 v1(mp1);
            LLVector3 v2(mp2);

            LLVector3 n = (v1-v0)%(v2-v0);
            n.normalize();

            res.mPositions.push_back(v0*scale);
            res.mPositions.push_back(v1*scale);
            res.mPositions.push_back(v2*scale);

            res.mNormals.push_back(n);
            res.mNormals.push_back(n);
            res.mNormals.push_back(n);
        }
    }
}

void RequestStats::updateTime()
{
    U32 modifier = 1 << mRetries; // before ++
    mRetries++;
    mTimer.reset();
    mTimer.setTimerExpirySec(DOWNLOAD_RETRY_DELAY * (F32)modifier); // up to 32s, 64 total wait
}

bool RequestStats::canRetry() const
{
    return mRetries < DOWNLOAD_RETRY_LIMIT;
}

bool RequestStats::isDelayed() const
{
    return mTimer.getStarted() && !mTimer.hasExpired();
}

F32 calculate_score(LLVOVolume* object)
{
    if (!object)
    {
        return -1.f;
    }
    LLDrawable* drawable = object->mDrawable;
    if (!drawable)
    {
        return -1;
    }
    if (drawable->isState(LLDrawable::RIGGED) || object->isAttachment())
    {
        LLVOAvatar* avatar = object->getAvatar();
        LLDrawable* av_drawable = avatar ? avatar->mDrawable : nullptr;
        if (avatar && av_drawable)
        {
            // See LLVOVolume::calcLOD()
            F32 radius;
            if (avatar->isControlAvatar())
            {
                const LLVector3* box = avatar->getLastAnimExtents();
                LLVector3 diag = box[1] - box[0];
                radius = diag.magVec() * 0.5f;
            }
            else
            {
                // Volume in a rigged mesh attached to a regular avatar.
                const LLVector3* box = avatar->getLastAnimExtents();
                LLVector3 diag = box[1] - box[0];
                radius = diag.magVec();

                if (!avatar->isSelf() && !avatar->hasFirstFullAttachmentData())
                {
                    // slightly deprioritize avatars that are still receiving data
                    radius *= 0.9f;
                }
            }
            return radius / llmax(av_drawable->mDistanceWRTCamera, 1.f);
        }
    }
    return drawable->getRadius() / llmax(drawable->mDistanceWRTCamera, 1.f);
}

void PendingRequestBase::updateScore()
{
    mScore = 0;
    if (mTrackedData)
    {
        for (LLVOVolume* volume : mTrackedData->mVolumes)
        {
            F32 cur_score = calculate_score(volume);
            if (cur_score > 0)
            {
                mScore = llmax(mScore, cur_score);
            }
        }
    }
}

LLViewerFetchedTexture* LLMeshUploadThread::FindViewerTexture(const LLImportMaterial& material)
{
    LLPointer< LLViewerFetchedTexture > * ppTex = static_cast< LLPointer< LLViewerFetchedTexture > * >(material.mOpaqueData);
    return ppTex ? (*ppTex).get() : NULL;
}

std::atomic<S32> LLMeshRepoThread::sActiveHeaderRequests = 0;
std::atomic<S32> LLMeshRepoThread::sActiveLODRequests = 0;
std::atomic<S32> LLMeshRepoThread::sActiveSkinRequests = 0;
U32 LLMeshRepoThread::sMaxConcurrentRequests = 1;
S32 LLMeshRepoThread::sRequestLowWater = REQUEST2_LOW_WATER_MIN;
S32 LLMeshRepoThread::sRequestHighWater = REQUEST2_HIGH_WATER_MIN;
S32 LLMeshRepoThread::sRequestWaterLevel = 0;

// Base handler class for all mesh users of llcorehttp.
// This is roughly equivalent to a Responder class in
// traditional LL code.  The base is going to perform
// common response/data handling in the inherited
// onCompleted() method.  Derived classes, one for each
// type of HTTP action, define processData() and
// processFailure() methods to customize handling and
// error messages.
//
// LLCore::HttpHandler
//   LLMeshHandlerBase
//     LLMeshHeaderHandler
//     LLMeshLODHandler
//     LLMeshSkinInfoHandler
//     LLMeshDecompositionHandler
//     LLMeshPhysicsShapeHandler
//   LLMeshUploadThread

class LLMeshHandlerBase : public LLCore::HttpHandler,
    public std::enable_shared_from_this<LLMeshHandlerBase>
{
public:
    typedef std::shared_ptr<LLMeshHandlerBase> ptr_t;

    LOG_CLASS(LLMeshHandlerBase);
    LLMeshHandlerBase(U32 offset, U32 requested_bytes)
        : LLCore::HttpHandler(),
          mMeshParams(),
          mProcessed(false),
          mHasDataOwnership(true),
          mHttpHandle(LLCORE_HTTP_HANDLE_INVALID),
          mOffset(offset),
          mRequestedBytes(requested_bytes)
        {}

    virtual ~LLMeshHandlerBase()
        {}

protected:
    LLMeshHandlerBase(const LLMeshHandlerBase &);               // Not defined
    void operator=(const LLMeshHandlerBase &);                  // Not defined

public:
    virtual void onCompleted(LLCore::HttpHandle handle, LLCore::HttpResponse * response);
    virtual void processData(LLCore::BufferArray * body, S32 body_offset, U8 * data, S32 data_size) = 0;
    virtual void processFailure(LLCore::HttpStatus status) = 0;

public:
    LLVolumeParams mMeshParams;
    bool mProcessed;
    LLCore::HttpHandle mHttpHandle;
    U32 mOffset;
    U32 mRequestedBytes;

protected:
    bool mHasDataOwnership = true;
};


// Subclass for header fetches.
//
// Thread:  repo
class LLMeshHeaderHandler : public LLMeshHandlerBase
{
public:
    LOG_CLASS(LLMeshHeaderHandler);
    LLMeshHeaderHandler(const LLVolumeParams & mesh_params, U32 offset, U32 requested_bytes)
        : LLMeshHandlerBase(offset, requested_bytes)
    {
        mMeshParams = mesh_params;
        LLMeshRepoThread::incActiveHeaderRequests();
    }
    virtual ~LLMeshHeaderHandler();

protected:
    LLMeshHeaderHandler(const LLMeshHeaderHandler &);           // Not defined
    void operator=(const LLMeshHeaderHandler &);                // Not defined

public:
    virtual void processData(LLCore::BufferArray * body, S32 body_offset, U8 * data, S32 data_size);
    virtual void processFailure(LLCore::HttpStatus status);
};


// Subclass for LOD fetches.
//
// Thread:  repo
class LLMeshLODHandler : public LLMeshHandlerBase
{
public:
    LOG_CLASS(LLMeshLODHandler);
    LLMeshLODHandler(const LLVolumeParams & mesh_params, S32 lod, U32 offset, U32 requested_bytes)
        : LLMeshHandlerBase(offset, requested_bytes),
          mLOD(lod)
    {
            mMeshParams = mesh_params;
            LLMeshRepoThread::incActiveLODRequests();
        }
    virtual ~LLMeshLODHandler();

protected:
    LLMeshLODHandler(const LLMeshLODHandler &);                 // Not defined
    void operator=(const LLMeshLODHandler &);                   // Not defined

public:
    virtual void processData(LLCore::BufferArray * body, S32 body_offset, U8 * data, S32 data_size);
    virtual void processFailure(LLCore::HttpStatus status);

private:
    void processLod(U8* data, S32 data_size);

public:
    S32 mLOD;
};


// Subclass for skin info fetches.
//
// Thread:  repo
class LLMeshSkinInfoHandler : public LLMeshHandlerBase
{
public:
    LOG_CLASS(LLMeshSkinInfoHandler);
    LLMeshSkinInfoHandler(const LLUUID& id, U32 offset, U32 requested_bytes)
        : LLMeshHandlerBase(offset, requested_bytes),
          mMeshID(id)
    {
        LLMeshRepoThread::incActiveSkinRequests();
    }
    virtual ~LLMeshSkinInfoHandler();

protected:
    LLMeshSkinInfoHandler(const LLMeshSkinInfoHandler &);       // Not defined
    void operator=(const LLMeshSkinInfoHandler &);              // Not defined

    void processSkin(U8* data, S32 data_size);

public:
    virtual void processData(LLCore::BufferArray * body, S32 body_offset, U8 * data, S32 data_size);
    virtual void processFailure(LLCore::HttpStatus status);

public:
    LLUUID mMeshID;
};


// Subclass for decomposition fetches.
//
// Thread:  repo
class LLMeshDecompositionHandler : public LLMeshHandlerBase
{
public:
    LOG_CLASS(LLMeshDecompositionHandler);
    LLMeshDecompositionHandler(const LLUUID& id, U32 offset, U32 requested_bytes)
        : LLMeshHandlerBase(offset, requested_bytes),
          mMeshID(id)
    {}
    virtual ~LLMeshDecompositionHandler();

protected:
    LLMeshDecompositionHandler(const LLMeshDecompositionHandler &);     // Not defined
    void operator=(const LLMeshDecompositionHandler &);                 // Not defined

public:
    virtual void processData(LLCore::BufferArray * body, S32 body_offset, U8 * data, S32 data_size);
    virtual void processFailure(LLCore::HttpStatus status);

public:
    LLUUID mMeshID;
};


// Subclass for physics shape fetches.
//
// Thread:  repo
class LLMeshPhysicsShapeHandler : public LLMeshHandlerBase
{
public:
    LOG_CLASS(LLMeshPhysicsShapeHandler);
    LLMeshPhysicsShapeHandler(const LLUUID& id, U32 offset, U32 requested_bytes)
        : LLMeshHandlerBase(offset, requested_bytes),
          mMeshID(id)
    {}
    virtual ~LLMeshPhysicsShapeHandler();

protected:
    LLMeshPhysicsShapeHandler(const LLMeshPhysicsShapeHandler &);   // Not defined
    void operator=(const LLMeshPhysicsShapeHandler &);              // Not defined

public:
    virtual void processData(LLCore::BufferArray * body, S32 body_offset, U8 * data, S32 data_size);
    virtual void processFailure(LLCore::HttpStatus status);

public:
    LLUUID mMeshID;
};


void log_upload_error(LLCore::HttpStatus status, const LLSD& content,
                      const char * const stage, const std::string & model_name)
{
    // Add notification popup.
    LLSD args;
    std::string message = content["error"]["message"].asString();
    std::string identifier = content["error"]["identifier"].asString();
    args["MESSAGE"] = message;
    args["IDENTIFIER"] = identifier;
    args["LABEL"] = model_name;

    // Log details.
    LL_WARNS(LOG_MESH) << "Error in stage:  " << stage
                       << ", Reason:  " << status.toString()
                       << " (" << status.toTerseString() << ")" << LL_ENDL;

    std::ostringstream details;
    typedef std::unordered_set<std::string> mav_errors_set_t;
    mav_errors_set_t mav_errors;

    if (content.has("error"))
    {
        const LLSD& err = content["error"];
        LL_WARNS(LOG_MESH) << "error: " << err << LL_ENDL;
        LL_WARNS(LOG_MESH) << "  mesh upload failed, stage '" << stage
                           << "', error '" << err["error"].asString()
                           << "', message '" << err["message"].asString()
                           << "', id '" << err["identifier"].asString()
                           << "'" << LL_ENDL;

        if (err.has("errors"))
        {
            details << std::endl << std::endl;

            S32 error_num = 0;
            const LLSD& err_list = err["errors"];
            for (LLSD::array_const_iterator it = err_list.beginArray();
                 it != err_list.endArray();
                 ++it)
            {
                const LLSD& err_entry = *it;
                std::string message = err_entry["message"];

                if (message.length() > 0)
                {
                    mav_errors.insert(message);
                }

                LL_WARNS(LOG_MESH) << "  error[" << error_num << "]:" << LL_ENDL;
                for (LLSD::map_const_iterator map_it = err_entry.beginMap();
                     map_it != err_entry.endMap();
                     ++map_it)
                {
                    LL_WARNS(LOG_MESH) << "    " << map_it->first << ":  "
                                       << map_it->second << LL_ENDL;
                }
                error_num++;
            }
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Bad response to mesh request, no additional error information available." << LL_ENDL;
    }

    mav_errors_set_t::iterator mav_errors_it = mav_errors.begin();
    for (; mav_errors_it != mav_errors.end(); ++mav_errors_it)
    {
        std::string mav_details = "Mav_Details_" + *mav_errors_it;
        // <FS:Ansariel> Details error can be some message already.
        //details << "Message: '" << *mav_errors_it << "': " << LLTrans::getString(mav_details) << std::endl << std::endl;
        std::string translated_details;
        if (LLTrans::findString(translated_details, mav_details))
        {
            details << "Message: '" << *mav_errors_it << "': " << translated_details << std::endl << std::endl;
        }
        else
        {
            details << "Message: '" << *mav_errors_it << "'" << std::endl << std::endl;
        }
        // </FS:Ansariel>
    }

    std::string details_str = details.str();
    if (details_str.length() > 0)
    {
        args["DETAILS"] = details_str;
    }

    gMeshRepo.uploadError(args);
}

void write_preamble(LLFileSystem &file, S32 header_bytes, S32 flags)
{
    LLMeshRepository::sCacheBytesWritten += CACHE_PREAMBLE_SIZE;
    file.write((U8*)&CACHE_PREAMBLE_VERSION, sizeof(U32));
    file.write((U8*)&header_bytes, sizeof(U32));
    file.write((U8*)&flags, sizeof(U32));
}

LLMeshRepoThread::LLMeshRepoThread()
: LLThread("mesh repo"),
  mHttpRequest(NULL),
  mHttpOptions(),
  mHttpLargeOptions(),
  mHttpHeaders(),
  mHttpPolicyClass(LLCore::HttpRequest::DEFAULT_POLICY_ID),
  mHttpLegacyPolicyClass(LLCore::HttpRequest::DEFAULT_POLICY_ID),
  // <FS:Ansariel> [UDP Assets]
  mHttpLargePolicyClass(LLCore::HttpRequest::DEFAULT_POLICY_ID),
  mLegacyGetMeshVersion(0),
  // <FS:Ansariel> [UDP Assets]
  mWorkQueue("MeshRepoThread", 1024*1024)
{
    LLAppCoreHttp & app_core_http(LLAppViewer::instance()->getAppCoreHttp());

    mMutex = new LLMutex();
    mHeaderMutex = new LLMutex();
    mLoadedMutex = new LLMutex();
    mPendingMutex = new LLMutex();
    mSkinMapMutex = new LLMutex();
    mSignal = new LLCondition();
    mHttpRequest = new LLCore::HttpRequest;
    mHttpOptions = LLCore::HttpOptions::ptr_t(new LLCore::HttpOptions);
    mHttpOptions->setTransferTimeout(SMALL_MESH_XFER_TIMEOUT);
    mHttpOptions->setUseRetryAfter(gSavedSettings.getBOOL("MeshUseHttpRetryAfter"));
    mHttpLargeOptions = LLCore::HttpOptions::ptr_t(new LLCore::HttpOptions);
    mHttpLargeOptions->setTransferTimeout(LARGE_MESH_XFER_TIMEOUT);
    mHttpLargeOptions->setUseRetryAfter(gSavedSettings.getBOOL("MeshUseHttpRetryAfter"));
    mHttpHeaders = LLCore::HttpHeaders::ptr_t(new LLCore::HttpHeaders);
    mHttpHeaders->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_VND_LL_MESH);
    mHttpPolicyClass = app_core_http.getPolicy(LLAppCoreHttp::AP_MESH2);
    mHttpLegacyPolicyClass = app_core_http.getPolicy(LLAppCoreHttp::AP_MESH1); // <FS:Ansariel> [UDP Assets]
    mHttpLargePolicyClass = app_core_http.getPolicy(LLAppCoreHttp::AP_LARGE_MESH);

    // Lod processing is expensive due to the number of requests
    // and a need to do expensive cacheOptimize().
    mMeshThreadPool.reset(new LL::ThreadPool("MeshLodProcessing", 2));
    mMeshThreadPool->start();
}


LLMeshRepoThread::~LLMeshRepoThread()
{
    LL_INFOS(LOG_MESH) << "Small GETs issued:  " << LLMeshRepository::sHTTPRequestCount
                       << ", Large GETs issued:  " << LLMeshRepository::sHTTPLargeRequestCount
                       << ", Max Lock Holdoffs:  " << LLMeshRepository::sMaxLockHoldoffs
                       << LL_ENDL;

    mHttpRequestSet.clear();
    mHttpHeaders.reset();

    while (!mSkinInfoQ.empty())
    {
        llassert(mSkinInfoQ.front()->getNumRefs() == 1);
        mSkinInfoQ.pop_front();
    }

    while (!mDecompositionQ.empty())
    {
        delete mDecompositionQ.front();
        mDecompositionQ.pop_front();
    }

    delete mHttpRequest;
    mHttpRequest = nullptr;
    delete mMutex;
    mMutex = nullptr;
    delete mHeaderMutex;
    mHeaderMutex = nullptr;
    delete mLoadedMutex;
    mLoadedMutex = nullptr;
    delete mPendingMutex;
    mPendingMutex = nullptr;
    delete mSkinMapMutex;
    mSkinMapMutex = nullptr;
    delete mSignal;
    mSignal = nullptr;
    delete[] mDiskCacheBuffer;
    mDiskCacheBuffer = nullptr;
}

void LLMeshRepoThread::run()
{
    LLCDResult res = LLConvexDecomposition::initThread();
    if (res != LLCD_OK && LLConvexDecomposition::isFunctional())
    {
        LL_WARNS(LOG_MESH) << "Convex decomposition unable to be loaded.  Expect severe problems." << LL_ENDL;
    }

    while (!LLApp::isExiting())
    {
        // *TODO:  Revise sleep/wake strategy and try to move away
        // from polling operations in this thread.  We can sleep
        // this thread hard when:
        // * All Http requests are serviced
        // * LOD request queue empty
        // * Header request queue empty
        // * Skin info request queue empty
        // * Decomposition request queue empty
        // * Physics shape request queue empty
        // We wake the thread when any of the above become untrue.
        // Will likely need a correctly-implemented condition variable to do this.
        // On the other hand, this may actually be an effective and efficient scheme...

        mSignal->wait();
        LL_PROFILE_ZONE_NAMED("mesh_thread_loop")

        if (LLApp::isExiting())
        {
            break;
        }

        // run mWorkQueue for up to 8ms
        static std::chrono::nanoseconds WorkTimeNanoSec{std::chrono::nanoseconds::rep(8 * 1000000) };
        mWorkQueue.runFor(WorkTimeNanoSec);

        if (! mHttpRequestSet.empty())
        {
            // Dispatch all HttpHandler notifications
            mHttpRequest->update(0L);
        }
        sRequestWaterLevel = static_cast<S32>(mHttpRequestSet.size());            // Stats data update

        // NOTE: order of queue processing intentionally favors LOD and Skin requests over header requests
        // Todo: we are processing mLODReqQ, mHeaderReqQ, mSkinRequests, mDecompositionRequests and mPhysicsShapeRequests
        // in relatively similar manners, remake code to simplify/unify the process,
        // like processRequests(&requestQ, fetchFunction); which does same thing for each element

        if (mHttpRequestSet.size() < sRequestHighWater
            && !mSkinRequests.empty())
        {
            if (!mSkinRequests.empty())
            {
                std::list<UUIDBasedRequest> incomplete;
                while (!mSkinRequests.empty() && mHttpRequestSet.size() < sRequestHighWater)
                {

                    mMutex->lock();
                    auto req = mSkinRequests.front();
                    mSkinRequests.pop_front();
                    mMutex->unlock();
                    if (req.isDelayed())
                    {
                        incomplete.emplace_back(req);
                    }
                    else if (!fetchMeshSkinInfo(req.mId))
                    {
                        if (req.canRetry())
                        {
                            req.updateTime();
                            incomplete.emplace_back(req);
                        }
                        else
                        {
                            LLMutexLock locker(mLoadedMutex);
                            mSkinUnavailableQ.push_back(req);
                            LL_DEBUGS() << "mSkinReqQ failed: " << req.mId << LL_ENDL;
                        }
                    }
                }

                if (!incomplete.empty())
                {
                    LLMutexLock locker(mMutex);
                    for (const auto& req : incomplete)
                    {
                        mSkinRequests.push_back(req);
                    }
                }
            }
        }

        if (!mLODReqQ.empty() && mHttpRequestSet.size() < sRequestHighWater)
        {
            std::list<LODRequest> incomplete;
            while (!mLODReqQ.empty() && mHttpRequestSet.size() < sRequestHighWater)
            {
                if (!mMutex)
                {
                    break;
                }

                mMutex->lock();
                LODRequest req = mLODReqQ.front();
                mLODReqQ.pop();
                LLMeshRepository::sLODProcessing--;
                mMutex->unlock();
                if (req.isDelayed())
                {
                    // failed to load before, wait a bit
                    incomplete.push_front(req);
                }
                else if (!fetchMeshLOD(req.mMeshParams, req.mLOD))
                {
                    if (req.canRetry())
                    {
                        // failed, resubmit
                        req.updateTime();
                        incomplete.push_front(req);
                    }
                    else
                    {
                        // too many fails
                        LLMutexLock lock(mLoadedMutex);
                        mUnavailableQ.push_back(req);
                        LL_WARNS() << "Failed to load " << req.mMeshParams << " , skip" << LL_ENDL;
                    }
                }
            }

            if (!incomplete.empty())
            {
                LLMutexLock locker(mMutex);
                for (std::list<LODRequest>::iterator iter = incomplete.begin(); iter != incomplete.end(); iter++)
                {
                    mLODReqQ.push(*iter);
                    ++LLMeshRepository::sLODProcessing;
                }
            }
        }

        if (!mHeaderReqQ.empty() && mHttpRequestSet.size() < sRequestHighWater)
        {
            std::list<HeaderRequest> incomplete;
            while (!mHeaderReqQ.empty() && mHttpRequestSet.size() < sRequestHighWater)
            {
                if (!mMutex)
                {
                    break;
                }

                mMutex->lock();
                HeaderRequest req = mHeaderReqQ.front();
                mHeaderReqQ.pop();
                mMutex->unlock();
                if (req.isDelayed())
                {
                    // failed to load before, wait a bit
                    incomplete.push_front(req);
                }
                else if (!fetchMeshHeader(req.mMeshParams))
                {
                    if (req.canRetry())
                    {
                        //failed, resubmit
                        req.updateTime();
                        incomplete.push_front(req);
                    }
                    else
                    {
                        LL_DEBUGS() << "mHeaderReqQ failed: " << req.mMeshParams << LL_ENDL;
                    }
                }
            }

            if (!incomplete.empty())
            {
                LLMutexLock locker(mMutex);
                for (std::list<HeaderRequest>::iterator iter = incomplete.begin(); iter != incomplete.end(); iter++)
                {
                    mHeaderReqQ.push(*iter);
                }
            }
        }

        // For the final three request lists, similar goal to above but
        // slightly different queue structures.  Stay off the mutex when
        // performing long-duration actions.

        if (mHttpRequestSet.size() < sRequestHighWater
            && (!mDecompositionRequests.empty()
            || !mPhysicsShapeRequests.empty()))
        {
            // Something to do probably, lock and double-check.  We don't want
            // to hold the lock long here.  That will stall main thread activities
            // so we bounce it.

            // holding lock, try next list
            // *TODO:  For UI/debug-oriented lists, we might drop the fine-
            // grained locking as there's a lowered expectation of smoothness
            // in these cases.
            if (!mDecompositionRequests.empty())
            {
                std::set<UUIDBasedRequest> incomplete;
                while (!mDecompositionRequests.empty() && mHttpRequestSet.size() < sRequestHighWater)
                {
                    mMutex->lock();
                    std::set<UUIDBasedRequest>::iterator iter = mDecompositionRequests.begin();
                    UUIDBasedRequest req = *iter;
                    mDecompositionRequests.erase(iter);
                    mMutex->unlock();
                    if (req.isDelayed())
                    {
                        incomplete.insert(req);
                    }
                    else if (!fetchMeshDecomposition(req.mId))
                    {
                        if (req.canRetry())
                        {
                            req.updateTime();
                            incomplete.insert(req);
                        }
                        else
                        {
                            LL_DEBUGS(LOG_MESH) << "mDecompositionRequests failed: " << req.mId << LL_ENDL;
                        }
                    }
                }

                if (!incomplete.empty())
                {
                    LLMutexLock locker(mMutex);
                    mDecompositionRequests.insert(incomplete.begin(), incomplete.end());
                }
            }

            // holding lock, final list
            if (!mPhysicsShapeRequests.empty())
            {
                std::set<UUIDBasedRequest> incomplete;
                while (!mPhysicsShapeRequests.empty() && mHttpRequestSet.size() < sRequestHighWater)
                {
                    mMutex->lock();
                    std::set<UUIDBasedRequest>::iterator iter = mPhysicsShapeRequests.begin();
                    UUIDBasedRequest req = *iter;
                    mPhysicsShapeRequests.erase(iter);
                    mMutex->unlock();
                    if (req.isDelayed())
                    {
                        incomplete.insert(req);
                    }
                    else if (!fetchMeshPhysicsShape(req.mId))
                    {
                        if (req.canRetry())
                        {
                            req.updateTime();
                            incomplete.insert(req);
                        }
                        else
                        {
                            LL_DEBUGS(LOG_MESH) << "mPhysicsShapeRequests failed: " << req.mId << LL_ENDL;
                        }
                    }
                }

                if (!incomplete.empty())
                {
                    LLMutexLock locker(mMutex);
                    mPhysicsShapeRequests.insert(incomplete.begin(), incomplete.end());
                }
            }
        }

        // For dev purposes only.  A dynamic change could make this false
        // and that shouldn't assert.
        // llassert_always(mHttpRequestSet.size() <= sRequestHighWater);
    }

    if (mSignal->isLocked())
    { //make sure to let go of the mutex associated with the given signal before shutting down
        mSignal->unlock();
    }

    res = LLConvexDecomposition::quitThread();
    if (res != LLCD_OK && LLConvexDecomposition::isFunctional())
    {
        LL_WARNS(LOG_MESH) << "Convex decomposition unable to be quit." << LL_ENDL;
    }
}
void LLMeshRepoThread::cleanup()
{
    mShuttingDown = true;
    mSignal->broadcast();
    mMeshThreadPool->close();
}

// Mutex:  LLMeshRepoThread::mMutex must be held on entry
void LLMeshRepoThread::loadMeshSkinInfo(const LLUUID& mesh_id)
{
    mSkinRequests.push_back(UUIDBasedRequest(mesh_id));
}

// Mutex:  LLMeshRepoThread::mMutex must be held on entry
void LLMeshRepoThread::loadMeshDecomposition(const LLUUID& mesh_id)
{
    mDecompositionRequests.insert(UUIDBasedRequest(mesh_id));
}

// Mutex:  LLMeshRepoThread::mMutex must be held on entry
void LLMeshRepoThread::loadMeshPhysicsShape(const LLUUID& mesh_id)
{
    mPhysicsShapeRequests.insert(UUIDBasedRequest(mesh_id));
}

void LLMeshRepoThread::lockAndLoadMeshLOD(const LLVolumeParams& mesh_params, S32 lod)
{
    if (!LLAppViewer::isExiting())
    {
        loadMeshLOD(mesh_params, lod);
    }
}

void LLMeshRepoThread::loadMeshLOD(const LLVolumeParams& mesh_params, S32 lod)
{ //could be called from any thread
    const LLUUID& mesh_id = mesh_params.getSculptID();
    loadMeshLOD(mesh_id, mesh_params, lod);
}

void LLMeshRepoThread::loadMeshLOD(const LLUUID& mesh_id, const LLVolumeParams& mesh_params, S32 lod)
{
    if (hasHeader(mesh_id))
    { //if we have the header, request LOD byte range

        LODRequest req(mesh_params, lod);
        {
            LLMutexLock lock(mMutex);
            mLODReqQ.push(req);
            LLMeshRepository::sLODProcessing++;
        }
    }
    else
    {
        LLMutexLock lock(mPendingMutex);
        HeaderRequest req(mesh_params);
        pending_lod_map::iterator pending = mPendingLOD.find(mesh_id);

        if (pending != mPendingLOD.end())
        {
            //append this lod request to existing header request
            if (lod < LLModel::NUM_LODS && lod >= 0)
            {
                pending->second[lod]++;
            }
            else
            {
                LL_WARNS(LOG_MESH) << "Invalid LOD request: " << lod << "for mesh" << mesh_id << LL_ENDL;
            }
            llassert_msg(lod < LLModel::NUM_LODS, "Requested lod is out of bounds");
        }
        else
        {
            //if no header request is pending, fetch header
            auto& array = mPendingLOD[mesh_id];
            std::fill(array.begin(), array.end(), 0);
            array[lod]++;

            LLMutexLock lock(mMutex);
            mHeaderReqQ.push(req);
        }
    }
}

U8* LLMeshRepoThread::getDiskCacheBuffer(S32 size)
{
    if (mDiskCacheBufferSize < size)
    {
        const S32 MINIMUM_BUFFER_SIZE = 8192; // a minimum to avoid frequent early reallocations
        size = llmax(MINIMUM_BUFFER_SIZE, size);
        delete[] mDiskCacheBuffer;
        try
        {
            mDiskCacheBuffer = new U8[size];
        }
        catch (std::bad_alloc&)
        {
            LL_WARNS(LOG_MESH) << "Failed to allocate memory for mesh thread's buffer, size: " << size << LL_ENDL;
            mDiskCacheBuffer = NULL;

            // Not sure what size is reasonable
            // but if 30MB allocation failed, we definitely have issues
            const S32 MAX_SIZE = 30 * 1024 * 1024; //30MB
            if (size < MAX_SIZE)
            {
                LLAppViewer::instance()->outOfMemorySoftQuit();
            } // else ignore failures for anomalously large data
        }
        mDiskCacheBufferSize = size;
    }
    else
    {
        // reusing old buffer, reset heading bytes to ensure
        // old content won't be parsable if something fails.
        memset(mDiskCacheBuffer, 0, 16);
    }
    return mDiskCacheBuffer;
}

// Mutex:  must be holding mMutex when called
// <FS:Ansariel> [UDP Assets]
//void LLMeshRepoThread::setGetMeshCap(const std::string & mesh_cap)
void LLMeshRepoThread::setGetMeshCap(const std::string & mesh_cap, const std::string & legacy_get_mesh1,
                                                                    const std::string & legacy_get_mesh2,
                                                                    int legacy_pref_version)
// </FS:Ansariel> [UDP Assets]
{
    // <FS:Ansariel> [UDP Assets]
    mLegacyGetMeshCapability = legacy_get_mesh1;
    mLegacyGetMesh2Capability = legacy_get_mesh2;
    mLegacyGetMeshVersion = legacy_pref_version;
    // </FS:Ansariel> [UDP Assets]
    mGetMeshCapability = mesh_cap;
}


// Constructs a Cap URL for the mesh.  Prefers a GetMesh2 cap
// over a GetMesh cap.
//
// Mutex:  acquires mMutex
// <FS:Ansariel> [UDP Assets]
//void LLMeshRepoThread::constructUrl(LLUUID mesh_id, std::string * url)
void LLMeshRepoThread::constructUrl(LLUUID mesh_id, std::string * url, int * legacy_version)
// </FS:Ansariel> [UDP Assets]
{
    std::string res_url;
    int res_version(0); // <FS:Ansariel> [UDP Assets]

    if (gAgent.getRegion())
    {
        {
            LLMutexLock lock(mMutex);
            // <FS:Ansariel> [UDP Assets]
            //res_url = mGetMeshCapability;
            if (!mGetMeshCapability.empty() && mLegacyGetMeshVersion == 0)
            {
                res_url = mGetMeshCapability;
            }
            else if (!mLegacyGetMesh2Capability.empty() && mLegacyGetMeshVersion > 1)
            {
                res_url = mLegacyGetMesh2Capability;
                res_version = 2;
            }
            else
            {
                res_url = mLegacyGetMeshCapability;
                res_version = 1;
            }
            // </FS:Ansariel> [UDP Assets]
        }

        if (!res_url.empty())
        {
            res_url += "/?mesh_id=";
            res_url += mesh_id.asString().c_str();
        }
        else
        {
            // <FS:Ansariel> [UDP Assets]
            //LL_WARNS_ONCE(LOG_MESH) << "Current region does not have ViewerAsset capability!  Cannot load meshes. Region id: "
            LL_WARNS_ONCE(LOG_MESH) << "Current region does not have ViewerAsset or GetMesh capability!  Cannot load "
            // </FS:Ansariel> [UDP Assets
                                    << gAgent.getRegion()->getRegionID() << LL_ENDL;
            LL_DEBUGS_ONCE(LOG_MESH) << "Cannot load mesh " << mesh_id << " due to missing capability." << LL_ENDL;
        }
    }
    else
    {
        LL_WARNS_ONCE(LOG_MESH) << "Current region is not loaded so there is no capability to load from! Cannot load meshes." << LL_ENDL;
        LL_DEBUGS_ONCE(LOG_MESH) << "Cannot load mesh " << mesh_id << " due to missing capability." << LL_ENDL;
    }

    *url = res_url;
    *legacy_version = res_version; // <FS:Ansariel> [UDP Assets]
}

// Issue an HTTP GET request with byte range using the right
// policy class.
//
// @return      Valid handle or LLCORE_HTTP_HANDLE_INVALID.
//              If the latter, actual status is found in
//              mHttpStatus member which is valid until the
//              next call to this method.
//
// Thread:  repo
// <FS:Ansariel> [UDP Assets]
//LLCore::HttpHandle LLMeshRepoThread::getByteRange(const std::string & url,
LLCore::HttpHandle LLMeshRepoThread::getByteRange(const std::string & url, int legacy_cap_version,
// </FS:Ansariel> [UDP Assets]
                                                  size_t offset, size_t len,
                                                  const LLCore::HttpHandler::ptr_t &handler)
{
    // Also used in lltexturefetch.cpp
    static LLCachedControl<bool> disable_range_req(gSavedSettings, "HttpRangeRequestsDisable", false);

    LLCore::HttpHandle handle(LLCORE_HTTP_HANDLE_INVALID);

    if (len < LARGE_MESH_FETCH_THRESHOLD)
    {
        // <FS:Ansariel> [UDP Assets]
        //handle = mHttpRequest->requestGetByteRange( mHttpPolicyClass,
        handle = mHttpRequest->requestGetByteRange( ((legacy_cap_version == 0 || legacy_cap_version == 2) ? mHttpPolicyClass : mHttpLegacyPolicyClass),
        // </FS:Ansariel> [UDP Assets]
                                                    url,
                                                    (disable_range_req ? size_t(0) : offset),
                                                    (disable_range_req ? size_t(0) : len),
                                                    mHttpOptions,
                                                    mHttpHeaders,
                                                    handler);
        if (LLCORE_HTTP_HANDLE_INVALID != handle)
        {
            ++LLMeshRepository::sHTTPRequestCount;
        }
    }
    else
    {
        handle = mHttpRequest->requestGetByteRange(mHttpLargePolicyClass,
                                                   url,
                                                   (disable_range_req ? size_t(0) : offset),
                                                   (disable_range_req ? size_t(0) : len),
                                                   mHttpLargeOptions,
                                                   mHttpHeaders,
                                                   handler);
        if (LLCORE_HTTP_HANDLE_INVALID != handle)
        {
            ++LLMeshRepository::sHTTPLargeRequestCount;
        }
    }
    if (LLCORE_HTTP_HANDLE_INVALID == handle)
    {
        // Something went wrong, capture the error code for caller.
        mHttpStatus = mHttpRequest->getStatus();
    }
    return handle;
}


bool LLMeshRepoThread::fetchMeshSkinInfo(const LLUUID& mesh_id)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!mHeaderMutex)
    {
        return false;
    }

    mHeaderMutex->lock();

    mesh_header_map::const_iterator header_it = mMeshHeader.find(mesh_id);
    if (header_it == mMeshHeader.end())
    { //we have no header info for this mesh, do nothing
        mHeaderMutex->unlock();
        return false;
    }

    ++LLMeshRepository::sMeshRequestCount;
    bool ret = true;
    const LLMeshHeader& header = header_it->second;
    U32 header_size = header.mHeaderSize;

    if (header_size > 0)
    {
        S32 version = header.mVersion;
        S32 offset = header_size + header.mSkinOffset;
        S32 size = header.mSkinSize;
        bool in_cache = header.mSkinInCache;

        mHeaderMutex->unlock();

        if (version <= MAX_MESH_VERSION && offset >= 0 && size > 0)
        {
            //check cache for mesh skin info
            S32 disk_ofset = offset + CACHE_PREAMBLE_SIZE;
            LLFileSystem file(mesh_id, LLAssetType::AT_MESH);
            if (in_cache && file.getSize() >= disk_ofset + size)
            {
                U8* buffer = new(std::nothrow) U8[size];
                if (!buffer)
                {
                    LL_WARNS(LOG_MESH) << "Failed to allocate memory for skin info, size: " << size << LL_ENDL;

                    // Not sure what size is reasonable for skin info,
                    // but if 30MB allocation failed, we definitely have issues
                    const S32 MAX_SIZE = 30 * 1024 * 1024; //30MB
                    if (size < MAX_SIZE)
                    {
                        LLAppViewer::instance()->outOfMemorySoftQuit();
                    } // else ignore failures for anomalously large data
                    LLMutexLock locker(mLoadedMutex);
                    mSkinUnavailableQ.emplace_back(mesh_id);
                    return true;
                }
                LLMeshRepository::sCacheBytesRead += size;
                ++LLMeshRepository::sCacheReads;
                file.seek(disk_ofset);
                file.read(buffer, size);

                //make sure buffer isn't all 0's by checking the first 1KB (reserved block but not written)
                bool zero = true;
                for (S32 i = 0; i < llmin(size, 1024) && zero; ++i)
                {
                    zero = buffer[i] == 0;
                }

                if (!zero)
                {
                    //attempt to parse
                    bool posted = mMeshThreadPool->getQueue().post(
                        [mesh_id, buffer, size]
                        ()
                    {
                        if (gMeshRepo.mThread->isShuttingDown())
                        {
                            delete[] buffer;
                            return;
                        }
                        if (!gMeshRepo.mThread->skinInfoReceived(mesh_id, buffer, size))
                        {
                            // either header is faulty or something else overwrote the cache
                            S32 header_size = 0;
                            U32 header_flags = 0;
                            {
                                LL_DEBUGS(LOG_MESH) << "Mesh header for ID " << mesh_id << " cache mismatch." << LL_ENDL;

                                LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);

                                auto header_it = gMeshRepo.mThread->mMeshHeader.find(mesh_id);
                                if (header_it != gMeshRepo.mThread->mMeshHeader.end())
                                {
                                    LLMeshHeader& header = header_it->second;
                                    // for safety just mark everything as missing
                                    header.mSkinInCache = false;
                                    header.mPhysicsConvexInCache = false;
                                    header.mPhysicsMeshInCache = false;
                                    for (S32 i = 0; i < LLModel::NUM_LODS; ++i)
                                    {
                                        header.mLodInCache[i] = false;
                                    }
                                    header_size = header.mHeaderSize;
                                    header_flags = header.getFlags();
                                }
                            }

                            if (header_size > 0)
                            {
                                LLFileSystem file(mesh_id, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);
                                if (file.getMaxSize() >= CACHE_PREAMBLE_SIZE)
                                {
                                    write_preamble(file, header_size, header_flags);
                                }
                            }

                            {
                                LLMutexLock lock(gMeshRepo.mThread->mMutex);
                                UUIDBasedRequest req(mesh_id);
                                gMeshRepo.mThread->mSkinRequests.push_back(req);
                            }
                        }
                        delete[] buffer;
                    });
                    if (posted)
                    {
                        // lambda owns buffer
                        return true;
                    }
                    else if (skinInfoReceived(mesh_id, buffer, size))
                    {
                        delete[] buffer;
                        return true;
                    }
                }
                delete[] buffer;
            }

            //reading from cache failed for whatever reason, fetch from sim
            std::string http_url;
            // <FS:Ansariel> [UDP Assets]
            //constructUrl(mesh_id, &http_url);
            int legacy_cap_version(0);
            constructUrl(mesh_id, &http_url, &legacy_cap_version);
            // </FS:Ansariel> [UDP Assets]

            if (!http_url.empty())
            {
                LLMeshHandlerBase::ptr_t handler(new LLMeshSkinInfoHandler(mesh_id, offset, size));
                // <FS:Ansariel> [UDP Assets]
                //LLCore::HttpHandle handle = getByteRange(http_url, offset, size, handler);
                LLCore::HttpHandle handle = getByteRange(http_url, legacy_cap_version, offset, size, handler);
                // </FS:Ansariel> [UDP Assets]
                if (LLCORE_HTTP_HANDLE_INVALID == handle)
                {
                    LL_WARNS(LOG_MESH) << "HTTP GET request failed for skin info on mesh " << mID
                                       << ".  Reason:  " << mHttpStatus.toString()
                                       << " (" << mHttpStatus.toTerseString() << ")"
                                       << LL_ENDL;
                    ret = false;
                }
                else
                {
                    handler->mHttpHandle = handle;
                    mHttpRequestSet.insert(handler);
                }
            }
            else
            {
                LLMutexLock locker(mLoadedMutex);
                mSkinUnavailableQ.emplace_back(mesh_id);
            }
        }
        else
        {
            LLMutexLock locker(mLoadedMutex);
            mSkinUnavailableQ.emplace_back(mesh_id);
        }
    }
    else
    {
        mHeaderMutex->unlock();
    }

    //early out was not hit, effectively fetched
    return ret;
}

bool LLMeshRepoThread::fetchMeshDecomposition(const LLUUID& mesh_id)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!mHeaderMutex)
    {
        return false;
    }

    mHeaderMutex->lock();

    auto header_it = mMeshHeader.find(mesh_id);
    if (header_it == mMeshHeader.end())
    { //we have no header info for this mesh, do nothing
        mHeaderMutex->unlock();
        return false;
    }

    ++LLMeshRepository::sMeshRequestCount;
    const auto& header = header_it->second;
    U32 header_size = header.mHeaderSize;
    bool ret = true;

    if (header_size > 0)
    {
        S32 version = header.mVersion;
        S32 offset = header_size + header.mPhysicsConvexOffset;
        S32 size = header.mPhysicsConvexSize;
        bool in_cache = header.mPhysicsConvexInCache;

        mHeaderMutex->unlock();

        if (version <= MAX_MESH_VERSION && offset >= 0 && size > 0)
        {
            // check cache for mesh decomposition
            S32 disk_ofset = offset + CACHE_PREAMBLE_SIZE;
            LLFileSystem file(mesh_id, LLAssetType::AT_MESH);
            if (in_cache && file.getSize() >= disk_ofset + size)
            {
                U8* buffer = getDiskCacheBuffer(size);
                if (!buffer)
                {
                    return true;
                }
                LLMeshRepository::sCacheBytesRead += size;
                ++LLMeshRepository::sCacheReads;

                file.seek(disk_ofset);
                file.read(buffer, size);

                //make sure buffer isn't all 0's by checking the first 1KB (reserved block but not written)
                bool zero = true;
                for (S32 i = 0; i < llmin(size, 1024) && zero; ++i)
                {
                    zero = buffer[i] == 0;
                }

                if (!zero)
                { //attempt to parse
                    if (decompositionReceived(mesh_id, buffer, size))
                    {
                        return true;
                    }
                }
            }

            //reading from cache failed for whatever reason, fetch from sim
            std::string http_url;
            // <FS:Ansariel> [UDP Assets]
            //constructUrl(mesh_id, &http_url);
            int legacy_cap_version(0);
            constructUrl(mesh_id, &http_url, &legacy_cap_version);
            // </FS:Ansariel> [UDP Assets]

            if (!http_url.empty())
            {
                LLMeshHandlerBase::ptr_t handler(new LLMeshDecompositionHandler(mesh_id, offset, size));
                // <FS:Ansariel> [UDP Assets]
                //LLCore::HttpHandle handle = getByteRange(http_url, offset, size, handler);
                LLCore::HttpHandle handle = getByteRange(http_url, legacy_cap_version, offset, size, handler);
                // </FS:Ansariel> [UDP Assets]
                if (LLCORE_HTTP_HANDLE_INVALID == handle)
                {
                    LL_WARNS(LOG_MESH) << "HTTP GET request failed for decomposition mesh " << mID
                                       << ".  Reason:  " << mHttpStatus.toString()
                                       << " (" << mHttpStatus.toTerseString() << ")"
                                       << LL_ENDL;
                    ret = false;
                }
                else
                {
                    handler->mHttpHandle = handle;
                    mHttpRequestSet.insert(handler);
                }
            }
        }
    }
    else
    {
        mHeaderMutex->unlock();
    }

    //early out was not hit, effectively fetched
    return ret;
}

bool LLMeshRepoThread::fetchMeshPhysicsShape(const LLUUID& mesh_id)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!mHeaderMutex)
    {
        return false;
    }

    mHeaderMutex->lock();

    auto header_it = mMeshHeader.find(mesh_id);
    if (header_it == mMeshHeader.end())
    { //we have no header info for this mesh, do nothing
        mHeaderMutex->unlock();
        return false;
    }

    ++LLMeshRepository::sMeshRequestCount;
    const auto& header = header_it->second;
    U32 header_size = header.mHeaderSize;
    bool ret = true;

    if (header_size > 0)
    {
        S32 version = header.mVersion;
        S32 offset = header_size + header.mPhysicsMeshOffset;
        S32 size = header.mPhysicsMeshSize;
        bool in_cache = header.mPhysicsMeshInCache;

        mHeaderMutex->unlock();

        // todo: check header.mHasPhysicsMesh
        if (version <= MAX_MESH_VERSION && offset >= 0 && size > 0)
        {
            //check cache for mesh physics shape info
            S32 disk_ofset = offset + CACHE_PREAMBLE_SIZE;
            LLFileSystem file(mesh_id, LLAssetType::AT_MESH);
            if (in_cache && file.getSize() >= disk_ofset +size)
            {
                LLMeshRepository::sCacheBytesRead += size;
                ++LLMeshRepository::sCacheReads;

                U8* buffer = getDiskCacheBuffer(size);
                if (!buffer)
                {
                    return true;
                }
                file.seek(disk_ofset);
                file.read(buffer, size);

                //make sure buffer isn't all 0's by checking the first 1KB (reserved block but not written)
                bool zero = true;
                for (S32 i = 0; i < llmin(size, 1024) && zero; ++i)
                {
                    zero = buffer[i] == 0;
                }

                if (!zero)
                { //attempt to parse
                    if (physicsShapeReceived(mesh_id, buffer, size) == MESH_OK)
                    {
                        return true;
                    }
                }
            }

            //reading from cache failed for whatever reason, fetch from sim
            std::string http_url;
            // <FS:Ansariel> [UDP Assets]
            //constructUrl(mesh_id, &http_url);
            int legacy_cap_version(0);
            constructUrl(mesh_id, &http_url, &legacy_cap_version);
            // </FS:Ansariel> [UDP Assets]

            if (!http_url.empty())
            {
                LLMeshHandlerBase::ptr_t handler(new LLMeshPhysicsShapeHandler(mesh_id, offset, size));
                // <FS:Ansariel> [UDP Assets]
                //LLCore::HttpHandle handle = getByteRange(http_url, offset, size, handler);
                LLCore::HttpHandle handle = getByteRange(http_url, legacy_cap_version, offset, size, handler);
                // </FS:Ansariel> [UDP Assets]
                if (LLCORE_HTTP_HANDLE_INVALID == handle)
                {
                    LL_WARNS(LOG_MESH) << "HTTP GET request failed for physics shape on mesh " << mID
                                       << ".  Reason:  " << mHttpStatus.toString()
                                       << " (" << mHttpStatus.toTerseString() << ")"
                                       << LL_ENDL;
                    ret = false;
                }
                else
                {
                    handler->mHttpHandle = handle;
                    mHttpRequestSet.insert(handler);
                }
            }
        }
        else
        { //no physics shape whatsoever, report back NULL
            physicsShapeReceived(mesh_id, NULL, 0);
        }
    }
    else
    {
        mHeaderMutex->unlock();
    }

    //early out was not hit, effectively fetched
    return ret;
}

//static
void LLMeshRepoThread::incActiveLODRequests()
{
    ++LLMeshRepoThread::sActiveLODRequests;
}

//static
void LLMeshRepoThread::decActiveLODRequests()
{
    --LLMeshRepoThread::sActiveLODRequests;
}

//static
void LLMeshRepoThread::incActiveHeaderRequests()
{
    ++LLMeshRepoThread::sActiveHeaderRequests;
}

//static
void LLMeshRepoThread::decActiveHeaderRequests()
{
    --LLMeshRepoThread::sActiveHeaderRequests;
}

//static
void LLMeshRepoThread::incActiveSkinRequests()
{
    ++LLMeshRepoThread::sActiveSkinRequests;
}

//static
void LLMeshRepoThread::decActiveSkinRequests()
{
    --LLMeshRepoThread::sActiveSkinRequests;
}

//return false if failed to get header
bool LLMeshRepoThread::fetchMeshHeader(const LLVolumeParams& mesh_params)
{
    LL_PROFILE_ZONE_SCOPED;
    ++LLMeshRepository::sMeshRequestCount;

    {
        //look for mesh in asset in cache
        LLFileSystem file(mesh_params.getSculptID(), LLAssetType::AT_MESH);

        S32 size = file.getSize();

        if (size > 0)
        {
            // *NOTE:  if the header size is ever more than 4KB, this will break
            constexpr S32 DISK_MINIMAL_READ = 4096;
            U8 buffer[DISK_MINIMAL_READ * 2];
            S32 bytes = llmin(size, DISK_MINIMAL_READ);
            LLMeshRepository::sCacheBytesRead += bytes;
            ++LLMeshRepository::sCacheReads;

            file.read(buffer, bytes);

            U32 version = 0;
            memcpy(&version, buffer, sizeof(U32));
            if (version == CACHE_PREAMBLE_VERSION)
            {
                S32 header_size = 0;
                memcpy(&header_size, buffer + sizeof(U32), sizeof(S32));
                if (header_size + CACHE_PREAMBLE_SIZE > DISK_MINIMAL_READ)
                {
                    bytes = llmin(size , DISK_MINIMAL_READ * 2);
                    file.read(buffer + DISK_MINIMAL_READ, bytes - DISK_MINIMAL_READ);
                }
                U32 flags = 0;
                memcpy(&flags, buffer + 2 * sizeof(U32), sizeof(U32));
                if (headerReceived(mesh_params, buffer + CACHE_PREAMBLE_SIZE, bytes - CACHE_PREAMBLE_SIZE, flags) == MESH_OK)
                {
                    LL_DEBUGS(LOG_MESH) << "Mesh/Cache: Mesh header for ID " << mesh_params.getSculptID() << " - was retrieved from the cache." << LL_ENDL;

                    // Found mesh in cache
                    return true;
                }
            }
        }
    }

    //either cache entry doesn't exist or is corrupt, request header from simulator
    bool retval = true;
    std::string http_url;
    // <FS:Ansariel> [UDP Assets]
    //constructUrl(mesh_params.getSculptID(), &http_url);
    int legacy_cap_version(0);
    constructUrl(mesh_params.getSculptID(), &http_url, &legacy_cap_version);
    // </FS:Ansariel> [UDP Assets]


    if (!http_url.empty())
    {
        LL_DEBUGS(LOG_MESH) << "Mesh/Cache: Mesh header for ID " << mesh_params.getSculptID() << " - was retrieved from the simulator." << LL_ENDL;

        //grab first 4KB if we're going to bother with a fetch.  Cache will prevent future fetches if a full mesh fits
        //within the first 4KB
        //NOTE -- this will break of headers ever exceed 4KB

        LLMeshHandlerBase::ptr_t handler(new LLMeshHeaderHandler(mesh_params, 0, MESH_HEADER_SIZE));
        // <FS:Ansariel> [UDP Assets]
        //LLCore::HttpHandle handle = getByteRange(http_url, 0, MESH_HEADER_SIZE, handler);
        LLCore::HttpHandle handle = getByteRange(http_url, legacy_cap_version, 0, MESH_HEADER_SIZE, handler);
        // </FS:Ansariel> [UDP Assets]
        if (LLCORE_HTTP_HANDLE_INVALID == handle)
        {
            LL_WARNS(LOG_MESH) << "HTTP GET request failed for mesh header " << mID
                               << ".  Reason:  " << mHttpStatus.toString()
                               << " (" << mHttpStatus.toTerseString() << ")"
                               << LL_ENDL;
            retval = false;
        }
        else
        {
            handler->mHttpHandle = handle;
            mHttpRequestSet.insert(handler);
        }
    }

    return retval;
}

//return false if failed to get mesh lod.
bool LLMeshRepoThread::fetchMeshLOD(const LLVolumeParams& mesh_params, S32 lod)
{
    LL_PROFILE_ZONE_SCOPED;
    if (!mHeaderMutex)
    {
        return false;
    }

    const LLUUID& mesh_id = mesh_params.getSculptID();

    mHeaderMutex->lock();
    auto header_it = mMeshHeader.find(mesh_id);
    if (header_it == mMeshHeader.end())
    { //we have no header info for this mesh, do nothing
        mHeaderMutex->unlock();
        return false;
    }
    ++LLMeshRepository::sMeshRequestCount;
    bool retval = true;

    const auto& header = header_it->second;
    U32 header_size = header.mHeaderSize;
    if (header_size > 0)
    {
        S32 version = header.mVersion;
        S32 offset = header_size + header.mLodOffset[lod];
        S32 size = header.mLodSize[lod];
        bool in_cache = header.mLodInCache[lod];
        mHeaderMutex->unlock();

        if (version <= MAX_MESH_VERSION && offset >= 0 && size > 0)
        {
            S32 disk_ofset = offset + CACHE_PREAMBLE_SIZE;
            //check cache for mesh asset
            LLFileSystem file(mesh_id, LLAssetType::AT_MESH);
            if (in_cache && (file.getSize() >= disk_ofset + size))
            {
                U8* buffer = new(std::nothrow) U8[size]; // todo, make buffer thread local and read in thread?
                if (!buffer)
                {
                    LL_WARNS(LOG_MESH) << "Can't allocate memory for mesh " << mesh_id << " LOD " << lod << ", size: " << size << LL_ENDL;

                    // Not sure what size is reasonable for a mesh,
                    // but if 30MB allocation failed, we definitely have issues
                    const S32 MAX_SIZE = 30 * 1024 * 1024; //30MB
                    if (size < MAX_SIZE)
                    {
                        LLAppViewer::instance()->outOfMemorySoftQuit();
                    } // else ignore failures for anomalously large data

                    LLMutexLock lock(mLoadedMutex);
                    mUnavailableQ.push_back(LODRequest(mesh_params, lod));
                    return true;
                }
                LLMeshRepository::sCacheBytesRead += size;
                ++LLMeshRepository::sCacheReads;
                file.seek(disk_ofset);
                file.read(buffer, size);

                //make sure buffer isn't all 0's by checking the first 1KB (reserved block but not written)
                bool zero = true;
                for (S32 i = 0; i < llmin(size, 1024) && zero; ++i)
                {
                    zero = buffer[i] == 0;
                }

                if (!zero)
                {
                    //attempt to parse
                    const LLVolumeParams params(mesh_params);
                    bool posted = mMeshThreadPool->getQueue().post(
                        [params, mesh_id, lod, buffer, size]
                        ()
                    {
                        if (gMeshRepo.mThread->isShuttingDown())
                        {
                            delete[] buffer;
                            return;
                        }
                        if (gMeshRepo.mThread->lodReceived(params, lod, buffer, size) == MESH_OK)
                        {
                            LL_DEBUGS(LOG_MESH) << "Mesh/Cache: Mesh body for ID " << mesh_id << " - was retrieved from the cache." << LL_ENDL;
                        }
                        else
                        {
                            // either header is faulty or something else overwrote the cache
                            S32 header_size = 0;
                            U32 header_flags = 0;
                            {
                                LL_DEBUGS(LOG_MESH) << "Mesh header for ID " << mesh_id << " cache mismatch." << LL_ENDL;

                                LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);

                                auto header_it = gMeshRepo.mThread->mMeshHeader.find(mesh_id);
                                if (header_it != gMeshRepo.mThread->mMeshHeader.end())
                                {
                                    LLMeshHeader& header = header_it->second;
                                    // for safety just mark everything as missing
                                    header.mSkinInCache = false;
                                    header.mPhysicsConvexInCache = false;
                                    header.mPhysicsMeshInCache = false;
                                    for (S32 i = 0; i < LLModel::NUM_LODS; ++i)
                                    {
                                        header.mLodInCache[i] = false;
                                    }
                                    header_size = header.mHeaderSize;
                                    header_flags = header.getFlags();
                                }
                            }

                            if (header_size > 0)
                            {
                                LLFileSystem file(mesh_id, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);
                                if (file.getMaxSize() >= CACHE_PREAMBLE_SIZE)
                                {
                                    write_preamble(file, header_size, header_flags);
                                }
                            }

                            {
                                LLMutexLock lock(gMeshRepo.mThread->mMutex);
                                LODRequest req(params, lod);
                                gMeshRepo.mThread->mLODReqQ.push(req);
                                LLMeshRepository::sLODProcessing++;
                            }
                        }
                        delete[] buffer;
                    });

                    if (posted)
                    {
                        // now lambda owns buffer
                        return true;
                    }
                    else if (lodReceived(mesh_params, lod, buffer, size) == MESH_OK)
                    {
                        delete[] buffer;
                        LL_DEBUGS(LOG_MESH) << "Mesh/Cache: Mesh body for ID " << mesh_id << " - was retrieved from the cache." << LL_ENDL;

                        return true;
                    }

                }

                delete[] buffer;
            }

            //reading from cache failed for whatever reason, fetch from sim
            std::string http_url;
            // <FS:Ansariel> [UDP Assets]
            //constructUrl(mesh_id, &http_url);
            int legacy_cap_version(0);
            constructUrl(mesh_id, &http_url, &legacy_cap_version);
            // </FS:Ansariel> [UDP Assets]

            if (!http_url.empty())
            {
                LL_DEBUGS(LOG_MESH) << "Mesh/Cache: Mesh body for ID " << mesh_id << " - was retrieved from the simulator." << LL_ENDL;

                LLMeshHandlerBase::ptr_t handler(new LLMeshLODHandler(mesh_params, lod, offset, size));
                // <FS:Ansariel> [UDP Assets]
                //LLCore::HttpHandle handle = getByteRange(http_url, offset, size, handler);
                LLCore::HttpHandle handle = getByteRange(http_url, legacy_cap_version, offset, size, handler);
                // </FS:Ansariel> [UDP Assets]
                if (LLCORE_HTTP_HANDLE_INVALID == handle)
                {
                    LL_WARNS(LOG_MESH) << "HTTP GET request failed for LOD on mesh " << mID
                                       << ".  Reason:  " << mHttpStatus.toString()
                                       << " (" << mHttpStatus.toTerseString() << ")"
                                       << LL_ENDL;
                    retval = false;
                }
                else
                {
                    // we already made a request, store the handle
                    handler->mHttpHandle = handle;
                    mHttpRequestSet.insert(handler);
                }
            }
            else
            {
                LLMutexLock lock(mLoadedMutex);
                mUnavailableQ.push_back(LODRequest(mesh_params, lod));
            }
        }
        else
        {
            LLMutexLock lock(mLoadedMutex);
            mUnavailableQ.push_back(LODRequest(mesh_params, lod));
        }
    }
    else
    {
        mHeaderMutex->unlock();
    }

    return retval;
}

EMeshProcessingResult LLMeshRepoThread::headerReceived(const LLVolumeParams& mesh_params, U8* data, S32 data_size, U32 flags)
{
    LL_PROFILE_ZONE_SCOPED;
    const LLUUID mesh_id = mesh_params.getSculptID();
    LLSD header_data;

    LLMeshHeader header;

    llssize header_size = 0;
    S32 skin_offset = -1;
    S32 skin_size = -1;
    S32 lod_offset[LLModel::NUM_LODS] = { -1 };
    S32 lod_size[LLModel::NUM_LODS] = { -1 };
    if (data_size > 0)
    {
        llssize dsize = data_size;
        char* result_ptr = strip_deprecated_header((char*)data, dsize, &header_size);

        data_size = (S32)dsize;

        boost::iostreams::stream<boost::iostreams::array_source> stream(result_ptr, data_size);

        if (!LLSDSerialize::fromBinary(header_data, stream, data_size))
        {
            LL_WARNS(LOG_MESH) << "Mesh header parse error.  Not a valid mesh asset!  ID:  " << mesh_id
                               << LL_ENDL;
            return MESH_PARSE_FAILURE;
        }

        if (!header_data.isMap())
        {
            LL_WARNS(LOG_MESH) << "Mesh header is invalid for ID: " << mesh_id << LL_ENDL;
            return MESH_INVALID;
        }

        header.fromLLSD(header_data);

        if (header.mVersion > MAX_MESH_VERSION)
        {
            LL_INFOS(LOG_MESH) << "Wrong version in header for " << mesh_id << LL_ENDL;
            header.m404 = true;
        }
        // make sure there is at least one lod, function returns -1 and marks as 404 otherwise
        else if (LLMeshRepository::getActualMeshLOD(header, 0) >= 0)
        {
            header.mHeaderSize = (S32)stream.tellg();
            header_size += header.mHeaderSize;
            skin_offset = header.mSkinOffset;
            skin_size = header.mSkinSize;

            memcpy(lod_offset, header.mLodOffset, sizeof(lod_offset));
            memcpy(lod_size, header.mLodSize, sizeof(lod_size));

            if (flags != 0)
            {
                header.setFromFlags(flags);
            }
            else
            {
                if (header.mSkinSize > 0 && header_size + header.mSkinOffset + header.mSkinSize < data_size)
                {
                    header.mSkinInCache = true;
                }
                if (header.mPhysicsConvexSize > 0 && header_size + header.mPhysicsConvexOffset + header.mPhysicsConvexSize < data_size)
                {
                    header.mPhysicsConvexInCache = true;
                }
                if (header.mPhysicsMeshSize > 0 && header_size + header.mPhysicsMeshOffset + header.mPhysicsMeshSize < data_size)
                {
                    header.mPhysicsMeshInCache = true;
                }
                for (S32 i = 0; i < LLModel::NUM_LODS; ++i)
                {
                    if (lod_size[i] > 0 && header_size + lod_offset[i] + lod_size[i] < data_size)
                    {
                        header.mLodInCache[i] = true;
                    }
                }
            }
        }
    }
    else
    {
        LL_INFOS(LOG_MESH) << "Non-positive data size.  Marking header as non-existent, will not retry.  ID:  " << mesh_id
                           << LL_ENDL;
        header.m404 = 1;
    }

    {

        {
            LLMutexLock lock(mHeaderMutex);
            mMeshHeader[mesh_id] = header;
            LLMeshRepository::sCacheBytesHeaders += (U32)header_size;
        }

        // immediately request SkinInfo since we'll need it before we can render any LoD if it is present
        if (skin_offset >= 0 && skin_size > 0)
        {
            {
                LLMutexLock lock(gMeshRepo.mMeshMutex);

                if (gMeshRepo.mLoadingSkins.find(mesh_id) == gMeshRepo.mLoadingSkins.end())
                {
                    gMeshRepo.mLoadingSkins[mesh_id]; // add an empty vector to indicate to main thread that we are loading skin info
                }
            }

            S32 offset = (S32)header_size + skin_offset;
            bool request_skin = true;
            if (offset + skin_size < data_size)
            {
                request_skin = !skinInfoReceived(mesh_id, data + offset, skin_size);
            }
            if (request_skin)
            {
                mSkinRequests.push_back(UUIDBasedRequest(mesh_id));
            }
        }

        std::array<S32, LLModel::NUM_LODS> pending_lods;
        bool has_pending_lods = false;
        {
            LLMutexLock lock(mPendingMutex); // make sure only one thread access mPendingLOD at the same time.
            pending_lod_map::iterator iter = mPendingLOD.find(mesh_id);
            if (iter != mPendingLOD.end())
            {
                pending_lods = iter->second;
                mPendingLOD.erase(iter);
                has_pending_lods = true;
            }
        }

        //check for pending requests
        if (has_pending_lods)
        {
            for (S32 i = 0; i < pending_lods.size(); ++i)
            {
                if (pending_lods[i] > 1)
                {
                    // mLoadingMeshes should be protecting from dupplciates, but looks
                    // like this is possible if object rezzes, unregisterMesh, then
                    // rezzes again before first request completes.
                    // mLoadingMeshes might need to change a bit to not rerequest if
                    // mesh is already pending.
                    //
                    // Todo: Improve mLoadingMeshes and once done turn this into an assert.
                    // Low priority since such situation should be relatively rare
                    LL_INFOS(LOG_MESH) << "Multiple dupplicate requests for mesd ID:  " << mesh_id << " LOD: " << i
                        << LL_ENDL;
                }
                if (pending_lods[i] > 0 && lod_size[i] > 0)
                {
                    // try to load from data we just received
                    bool request_lod = true;
                    S32 offset = (S32)header_size + lod_offset[i];
                    if (offset + lod_size[i] <= data_size)
                    {
                        // initial request is 4096 bytes, it's big enough to fit this lod
                        request_lod = lodReceived(mesh_params, i, data + offset, lod_size[i]) != MESH_OK;
                    }
                    if (request_lod)
                    {
                        LLMutexLock lock(mMutex);
                        LODRequest req(mesh_params, i);
                        mLODReqQ.push(req);
                        LLMeshRepository::sLODProcessing++;
                    }
                }
            }
        }
    }

    return MESH_OK;
}

EMeshProcessingResult LLMeshRepoThread::lodReceived(const LLVolumeParams& mesh_params, S32 lod, U8* data, S32 data_size)
{
    if (data == NULL || data_size == 0)
    {
        return MESH_NO_DATA;
    }

    LLPointer<LLVolume> volume = new LLVolume(mesh_params, LLVolumeLODGroup::getVolumeScaleFromDetail(lod));
    if (volume->unpackVolumeFaces(data, data_size))
    {
        if (volume->getNumFaces() > 0)
        {
            // if we have a valid SkinInfo, cache per-joint bounding boxes for this LOD
            LLPointer<LLMeshSkinInfo> skin_info = nullptr;
            {
                LLMutexLock lock(mSkinMapMutex);
                skin_map::iterator iter = mSkinMap.find(mesh_params.getSculptID());
                if (iter != mSkinMap.end())
                {
                    skin_info = iter->second;
                }
            }
            if (skin_info.notNull() && isAgentAvatarValid())
            {
                for (S32 i = 0; i < volume->getNumFaces(); ++i)
                {
                    // NOTE: no need to lock gAgentAvatarp as the state being checked is not changed after initialization
                    LLVolumeFace& face = volume->getVolumeFace(i);
                    LLSkinningUtil::updateRiggingInfo(skin_info, gAgentAvatarp, face);
                }
            }

            LoadedMesh mesh(volume, mesh_params, lod);
            {
                LLMutexLock lock(mLoadedMutex);
                mLoadedQ.push_back(mesh);
                // LLPointer is not thread safe, since we added this pointer into
                // threaded list, make sure counter gets decreased inside mutex lock
                // and won't affect mLoadedQ processing
                volume = NULL;
                // might be good idea to turn mesh into pointer to avoid making a copy
                mesh.mVolume = NULL;
            }
            return MESH_OK;
        }
    }

    return MESH_UNKNOWN;
}

bool LLMeshRepoThread::skinInfoReceived(const LLUUID& mesh_id, U8* data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    LLSD skin;

    if (data_size > 0)
    {
        try
        {
            U32 uzip_result = LLUZipHelper::unzip_llsd(skin, data, data_size);
            if (uzip_result != LLUZipHelper::ZR_OK)
            {
                LL_WARNS(LOG_MESH) << "Mesh skin info parse error.  Not a valid mesh asset!  ID:  " << mesh_id
                    << " uzip result" << uzip_result
                    << LL_ENDL;
                return false;
            }
        }
        catch (std::bad_alloc&)
        {
            LL_WARNS(LOG_MESH) << "Out of memory for mesh ID " << mesh_id << " of size: " << data_size << LL_ENDL;
            return false;
        }
    }

    {
        LLPointer<LLMeshSkinInfo> info = nullptr;
        info = new LLMeshSkinInfo(mesh_id, skin);

        if (isAgentAvatarValid())
        { // joint numbers are consistent inside LLVOAvatar and animations, but inconsistent inside meshes,
            // generate a map of mesh joint numbers to LLVOAvatar joint numbers
            LLSkinningUtil::initJointNums(info, gAgentAvatarp);
        }

        // copy the skin info for the background thread so we can use it
        // to calculate per-joint bounding boxes when volumes are loaded
        {
            LLMutexLock lock(mSkinMapMutex);
            mSkinMap[mesh_id] = new LLMeshSkinInfo(*info);
        }

        {
            // Move the LLPointer in to the skin info queue to avoid reference
            // count modification after we leave the lock
            LLMutexLock lock(mLoadedMutex);
            mSkinInfoQ.emplace_back(std::move(info));
        }
    }

    return true;
}

bool LLMeshRepoThread::decompositionReceived(const LLUUID& mesh_id, U8* data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    LLSD decomp;

    if (data_size > 0)
    {
        try
        {
            U32 uzip_result = LLUZipHelper::unzip_llsd(decomp, data, data_size);
            if (uzip_result != LLUZipHelper::ZR_OK)
            {
                LL_WARNS(LOG_MESH) << "Mesh decomposition parse error.  Not a valid mesh asset!  ID:  " << mesh_id
                    << " uzip result: " << uzip_result
                    << LL_ENDL;
                return false;
            }
        }
        catch (const std::bad_alloc&)
        {
            LL_WARNS(LOG_MESH) << "Out of memory for mesh ID " << mesh_id << " of size: " << data_size << LL_ENDL;
            return false;
        }
    }

    {
        LLModel::Decomposition* d = new LLModel::Decomposition(decomp);
        d->mMeshID = mesh_id;
        {
            LLMutexLock lock(mLoadedMutex);
            mDecompositionQ.push_back(d);
        }
    }

    return true;
}

EMeshProcessingResult LLMeshRepoThread::physicsShapeReceived(const LLUUID& mesh_id, U8* data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    LLSD physics_shape;

    LLModel::Decomposition* d = new LLModel::Decomposition();
    d->mMeshID = mesh_id;

    if (data == NULL)
    { //no data, no physics shape exists
        d->mPhysicsShapeMesh.clear();
    }
    else
    {
        LLVolumeParams volume_params;
        volume_params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
        volume_params.setSculptID(mesh_id, LL_SCULPT_TYPE_MESH);
        LLPointer<LLVolume> volume = new LLVolume(volume_params,0);

        if (volume->unpackVolumeFaces(data, data_size))
        {
            d->mPhysicsShapeMesh.clear();

            std::vector<LLVector3>& pos = d->mPhysicsShapeMesh.mPositions;
            std::vector<LLVector3>& norm = d->mPhysicsShapeMesh.mNormals;

            for (S32 i = 0; i < volume->getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& face = volume->getVolumeFace(i);

                for (S32 i = 0; i < face.mNumIndices; ++i)
                {
                    U16 idx = face.mIndices[i];

                    pos.push_back(LLVector3(face.mPositions[idx].getF32ptr()));
                    norm.push_back(LLVector3(face.mNormals[idx].getF32ptr()));
                }
            }
        }
    }

    {
        LLMutexLock lock(mLoadedMutex);
        mDecompositionQ.push_back(d);
    }
    return MESH_OK;
}

LLMeshUploadThread::LLMeshUploadThread(LLMeshUploadThread::instance_list& data, LLVector3& scale, bool upload_textures,
                                       bool upload_skin, bool upload_joints, bool lock_scale_if_joint_position,
                                       const std::string & upload_url, bool do_upload,
                                       LLHandle<LLWholeModelFeeObserver> fee_observer,
                                       LLHandle<LLWholeModelUploadObserver> upload_observer)
  : LLThread("mesh upload"),
    LLCore::HttpHandler(),
    mDiscarded(false),
    mDoUpload(do_upload),
    mWholeModelUploadURL(upload_url),
    mFeeObserverHandle(fee_observer),
    mUploadObserverHandle(upload_observer)
{
    mInstanceList = data;
    mUploadTextures = upload_textures;
    mUploadSkin = upload_skin;
    mUploadJoints = upload_joints;
    mLockScaleIfJointPosition = lock_scale_if_joint_position;
    mMutex = new LLMutex();
    mPendingUploads = 0;
    mFinished = false;
    mOrigin = gAgent.getPositionAgent();
    mHost = gAgent.getRegionHost();

    mWholeModelFeeCapability = gAgent.getRegionCapability("NewFileAgentInventory");

    mOrigin += gAgent.getAtAxis() * scale.magVec();

    mMeshUploadTimeOut = gSavedSettings.getS32("MeshUploadTimeOut");

    mHttpRequest = new LLCore::HttpRequest;
    mHttpOptions = LLCore::HttpOptions::ptr_t(new LLCore::HttpOptions);
    mHttpOptions->setTransferTimeout(mMeshUploadTimeOut);
    mHttpOptions->setUseRetryAfter(gSavedSettings.getBOOL("MeshUseHttpRetryAfter"));
    mHttpOptions->setRetries(UPLOAD_RETRY_LIMIT);
    mHttpHeaders = LLCore::HttpHeaders::ptr_t(new LLCore::HttpHeaders);
    mHttpHeaders->append(HTTP_OUT_HEADER_CONTENT_TYPE, HTTP_CONTENT_LLSD_XML);
    mHttpPolicyClass = LLAppViewer::instance()->getAppCoreHttp().getPolicy(LLAppCoreHttp::AP_UPLOADS);
}

LLMeshUploadThread::~LLMeshUploadThread()
{
    delete mHttpRequest;
    mHttpRequest = NULL;
    delete mMutex;
    mMutex = NULL;
}

LLMeshUploadThread::DecompRequest::DecompRequest(LLModel* mdl, LLModel* base_model, LLMeshUploadThread* thread)
{
    mStage = "single_hull";
    mModel = mdl;
    mDecompID = &mdl->mDecompID;
    mBaseModel = base_model;
    mThread = thread;

    //copy out positions and indices
    assignData(mdl) ;

    mThread->mFinalDecomp = this;
    mThread->mPhysicsComplete = false;
}

void LLMeshUploadThread::DecompRequest::completed()
{
    if (mThread->mFinalDecomp == this)
    {
        mThread->mPhysicsComplete = true;
    }

    llassert(mHull.size() == 1);

    mThread->mHullMap[mBaseModel] = mHull[0];
}

//called in the main thread.
void LLMeshUploadThread::preStart()
{
    //build map of LLModel refs to instances for callbacks
    for (instance_list::iterator iter = mInstanceList.begin(); iter != mInstanceList.end(); ++iter)
    {
        mInstance[iter->mModel].push_back(*iter);
    }
}

void LLMeshUploadThread::discard()
{
    LLMutexLock lock(mMutex);
    mDiscarded = true;
}

bool LLMeshUploadThread::isDiscarded() const
{
    LLMutexLock lock(mMutex);
    return mDiscarded;
}

void LLMeshUploadThread::run()
{
    if (mDoUpload)
    {
        doWholeModelUpload();
    }
    else
    {
        requestWholeModelFee();
    }
}

void dump_llsd_to_file(const LLSD& content, std::string filename)
{
    if (gSavedSettings.getBOOL("MeshUploadLogXML"))
    {
        llofstream of(filename.c_str());
        LLSDSerialize::toPrettyXML(content,of);
    }
}

LLSD llsd_from_file(std::string filename)
{
    llifstream ifs(filename.c_str());
    LLSD result;
    LLSDSerialize::fromXML(result,ifs);
    return result;
}

void LLMeshUploadThread::wholeModelToLLSD(LLSD& dest, bool include_textures)
{
    LLSD result;

    LLSD res;
    result["folder_id"] = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
    result["texture_folder_id"] = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_TEXTURE);
    result["asset_type"] = "mesh";
    result["inventory_type"] = "object";
    result["description"] = "(No Description)";
    result["next_owner_mask"] = LLSD::Integer(LLFloaterPerms::getNextOwnerPerms("Uploads"));
    result["group_mask"] = LLSD::Integer(LLFloaterPerms::getGroupPerms("Uploads"));
    result["everyone_mask"] = LLSD::Integer(LLFloaterPerms::getEveryonePerms("Uploads"));

    res["mesh_list"] = LLSD::emptyArray();
    res["texture_list"] = LLSD::emptyArray();
    res["instance_list"] = LLSD::emptyArray();
    S32 mesh_num = 0;
    S32 texture_num = 0;

    std::unordered_set<LLViewerTexture* > textures;
    std::unordered_map<LLViewerTexture*,S32> texture_index;

    std::unordered_map<LLModel*,S32> mesh_index;
    std::string model_name;

    S32 instance_num = 0;

    for (instance_map::iterator iter = mInstance.begin(); iter != mInstance.end(); ++iter)
    {
        LLMeshUploadData data;
        data.mBaseModel = iter->first;

        if (data.mBaseModel->mSubmodelID)
        {
            // These are handled below to insure correct parenting order on creation
            // due to map walking being based on model address (aka random)
            continue;
        }

        LLModelInstance& first_instance = *(iter->second.begin());
        for (S32 i = 0; i < 5; i++)
        {
            data.mModel[i] = first_instance.mLOD[i];
        }

        if (mesh_index.find(data.mBaseModel) == mesh_index.end())
        {
            // Have not seen this model before - create a new mesh_list entry for it.
            if (model_name.empty())
            {
                model_name = data.mBaseModel->getName();
            }

            std::stringstream ostr;

            LLModel::Decomposition& decomp =
                data.mModel[LLModel::LOD_PHYSICS].notNull() ?
                data.mModel[LLModel::LOD_PHYSICS]->mPhysics :
                data.mBaseModel->mPhysics;

            decomp.mBaseHull = mHullMap[data.mBaseModel];

            LLSD mesh_header = LLModel::writeModel(
                ostr,
                data.mModel[LLModel::LOD_PHYSICS],
                data.mModel[LLModel::LOD_HIGH],
                data.mModel[LLModel::LOD_MEDIUM],
                data.mModel[LLModel::LOD_LOW],
                data.mModel[LLModel::LOD_IMPOSTOR],
                decomp,
                mUploadSkin,
                mUploadJoints,
                mLockScaleIfJointPosition,
                false,
                false,
                data.mBaseModel->mSubmodelID);

            data.mAssetData = ostr.str();
            std::string str = ostr.str();

            res["mesh_list"][mesh_num] = LLSD::Binary(str.begin(),str.end());
            mesh_index[data.mBaseModel] = mesh_num;
            mesh_num++;
        }

        // For all instances that use this model
        for (instance_list::iterator instance_iter = iter->second.begin();
             instance_iter != iter->second.end();
             ++instance_iter)
        {

            LLModelInstance& instance = *instance_iter;

            LLSD instance_entry;

            for (S32 i = 0; i < 5; i++)
            {
                data.mModel[i] = instance.mLOD[i];
            }

            LLVector3 pos, scale;
            LLQuaternion rot;
            LLMatrix4 transformation = instance.mTransform;
            decomposeMeshMatrix(transformation,pos,rot,scale);
            instance_entry["position"] = ll_sd_from_vector3(pos);
            instance_entry["rotation"] = ll_sd_from_quaternion(rot);
            instance_entry["scale"] = ll_sd_from_vector3(scale);

            instance_entry["material"] = LL_MCODE_WOOD;
            instance_entry["physics_shape_type"] = data.mModel[LLModel::LOD_PHYSICS].notNull() ? (U8)(LLViewerObject::PHYSICS_SHAPE_PRIM) : (U8)(LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
            instance_entry["mesh"] = mesh_index[data.mBaseModel];
            instance_entry["mesh_name"] = instance.mLabel;

            instance_entry["face_list"] = LLSD::emptyArray();

            // We want to be able to allow more than 8 materials...
            //
            S32 end = llmin((S32)data.mBaseModel->mMaterialList.size(), instance.mModel->getNumVolumeFaces()) ;

            for (S32 face_num = 0; face_num < end; face_num++)
            {
                // multiple faces can reuse the same material
                LLImportMaterial& material = instance.mMaterial[data.mBaseModel->mMaterialList[face_num]];
                LLSD face_entry = LLSD::emptyMap();

                LLViewerFetchedTexture *texture = NULL;

                if (material.mDiffuseMapFilename.size())
                {
                    texture = FindViewerTexture(material);
                }

                if ((texture != NULL) &&
                    (textures.find(texture) == textures.end()))
                {
                    textures.insert(texture);
                }

                std::stringstream texture_str;
                if (texture != NULL && include_textures && mUploadTextures)
                {
                    if (texture->hasSavedRawImage())
                    {
                        LLImageDataLock lock(texture->getSavedRawImage());

                        LLPointer<LLImageJ2C> upload_file =
                            LLViewerTextureList::convertToUploadFile(texture->getSavedRawImage());

                        if (!upload_file.isNull() && upload_file->getDataSize())
                        {
                            texture_str.write((const char*) upload_file->getData(), upload_file->getDataSize());
                        }
                    }
                }

                if (texture != NULL &&
                    mUploadTextures &&
                    texture_index.find(texture) == texture_index.end())
                {
                    texture_index[texture] = texture_num;
                    std::string str = texture_str.str();
                    res["texture_list"][texture_num] = LLSD::Binary(str.begin(),str.end());
                    texture_num++;
                }

                // Subset of TextureEntry fields.
                if (texture != NULL && mUploadTextures)
                {
                    face_entry["image"] = texture_index[texture];
                    face_entry["scales"] = 1.0;
                    face_entry["scalet"] = 1.0;
                    face_entry["offsets"] = 0.0;
                    face_entry["offsett"] = 0.0;
                    face_entry["imagerot"] = 0.0;
                }
                face_entry["diffuse_color"] = ll_sd_from_color4(material.mDiffuseColor);
                face_entry["fullbright"] = material.mFullbright;
                instance_entry["face_list"][face_num] = face_entry;
            }

            res["instance_list"][instance_num] = instance_entry;
            instance_num++;
        }
    }

    for (instance_map::iterator iter = mInstance.begin(); iter != mInstance.end(); ++iter)
    {
        LLMeshUploadData data;
        data.mBaseModel = iter->first;

        if (!data.mBaseModel->mSubmodelID)
        {
            // These were handled above already...
            //
            continue;
        }

        LLModelInstance& first_instance = *(iter->second.begin());
        for (S32 i = 0; i < 5; i++)
        {
            data.mModel[i] = first_instance.mLOD[i];
        }

        if (mesh_index.find(data.mBaseModel) == mesh_index.end())
        {
            // Have not seen this model before - create a new mesh_list entry for it.
            if (model_name.empty())
            {
                model_name = data.mBaseModel->getName();
            }

            std::stringstream ostr;

            LLModel::Decomposition& decomp =
                data.mModel[LLModel::LOD_PHYSICS].notNull() ?
                data.mModel[LLModel::LOD_PHYSICS]->mPhysics :
                data.mBaseModel->mPhysics;

            decomp.mBaseHull = mHullMap[data.mBaseModel];

            LLSD mesh_header = LLModel::writeModel(
                ostr,
                data.mModel[LLModel::LOD_PHYSICS],
                data.mModel[LLModel::LOD_HIGH],
                data.mModel[LLModel::LOD_MEDIUM],
                data.mModel[LLModel::LOD_LOW],
                data.mModel[LLModel::LOD_IMPOSTOR],
                decomp,
                mUploadSkin,
                mUploadJoints,
                mLockScaleIfJointPosition,
                false,
                false,
                data.mBaseModel->mSubmodelID);

            data.mAssetData = ostr.str();
            std::string str = ostr.str();

            res["mesh_list"][mesh_num] = LLSD::Binary(str.begin(),str.end());
            mesh_index[data.mBaseModel] = mesh_num;
            mesh_num++;
        }

        // For all instances that use this model
        for (instance_list::iterator instance_iter = iter->second.begin();
             instance_iter != iter->second.end();
             ++instance_iter)
        {

            LLModelInstance& instance = *instance_iter;

            LLSD instance_entry;

            for (S32 i = 0; i < 5; i++)
            {
                data.mModel[i] = instance.mLOD[i];
            }

            LLVector3 pos, scale;
            LLQuaternion rot;
            LLMatrix4 transformation = instance.mTransform;
            decomposeMeshMatrix(transformation,pos,rot,scale);
            instance_entry["position"] = ll_sd_from_vector3(pos);
            instance_entry["rotation"] = ll_sd_from_quaternion(rot);
            instance_entry["scale"] = ll_sd_from_vector3(scale);

            instance_entry["material"] = LL_MCODE_WOOD;
            instance_entry["physics_shape_type"] = (U8)(LLViewerObject::PHYSICS_SHAPE_NONE);
            instance_entry["mesh"] = mesh_index[data.mBaseModel];

            instance_entry["face_list"] = LLSD::emptyArray();

            // We want to be able to allow more than 8 materials...
            //
            S32 end = llmin((S32)instance.mMaterial.size(), instance.mModel->getNumVolumeFaces()) ;

            for (S32 face_num = 0; face_num < end; face_num++)
            {
                LLImportMaterial& material = instance.mMaterial[data.mBaseModel->mMaterialList[face_num]];
                LLSD face_entry = LLSD::emptyMap();

                LLViewerFetchedTexture *texture = NULL;

                if (material.mDiffuseMapFilename.size())
                {
                    texture = FindViewerTexture(material);
                }

                if ((texture != NULL) &&
                    (textures.find(texture) == textures.end()))
                {
                    textures.insert(texture);
                }

                std::stringstream texture_str;
                if (texture != NULL && include_textures && mUploadTextures)
                {
                    if (texture->hasSavedRawImage())
                    {
                        LLImageDataLock lock(texture->getSavedRawImage());

                        LLPointer<LLImageJ2C> upload_file =
                            LLViewerTextureList::convertToUploadFile(texture->getSavedRawImage());

                        if (!upload_file.isNull() && upload_file->getDataSize())
                        {
                            texture_str.write((const char*) upload_file->getData(), upload_file->getDataSize());
                        }
                    }
                }

                if (texture != NULL &&
                    mUploadTextures &&
                    texture_index.find(texture) == texture_index.end())
                {
                    texture_index[texture] = texture_num;
                    std::string str = texture_str.str();
                    res["texture_list"][texture_num] = LLSD::Binary(str.begin(),str.end());
                    texture_num++;
                }

                // Subset of TextureEntry fields.
                if (texture != NULL && mUploadTextures)
                {
                    face_entry["image"] = texture_index[texture];
                    face_entry["scales"] = 1.0;
                    face_entry["scalet"] = 1.0;
                    face_entry["offsets"] = 0.0;
                    face_entry["offsett"] = 0.0;
                    face_entry["imagerot"] = 0.0;
                }
                face_entry["diffuse_color"] = ll_sd_from_color4(material.mDiffuseColor);
                face_entry["fullbright"] = material.mFullbright;
                instance_entry["face_list"][face_num] = face_entry;
            }

            res["instance_list"][instance_num] = instance_entry;
            instance_num++;
        }
    }

    if (model_name.empty()) model_name = "mesh model";
    result["name"] = model_name;
    res["metric"] = "MUT_Unspecified";
    result["asset_resources"] = res;
    dump_llsd_to_file(result,make_dump_name("whole_model_",dump_num));

    dest = result;
}

void LLMeshUploadThread::generateHulls()
{
    bool has_valid_requests = false ;

    for (instance_map::iterator iter = mInstance.begin(); iter != mInstance.end(); ++iter)
    {
        LLMeshUploadData data;
        data.mBaseModel = iter->first;

        LLModelInstance& instance = *(iter->second.begin());

        for (S32 i = 0; i < 5; i++)
        {
            data.mModel[i] = instance.mLOD[i];
        }

        //queue up models for hull generation
        LLModel* physics = NULL;

        if (data.mModel[LLModel::LOD_PHYSICS].notNull())
        {
            physics = data.mModel[LLModel::LOD_PHYSICS];
        }
        else if (data.mModel[LLModel::LOD_LOW].notNull())
        {
            physics = data.mModel[LLModel::LOD_LOW];
        }
        else if (data.mModel[LLModel::LOD_MEDIUM].notNull())
        {
            physics = data.mModel[LLModel::LOD_MEDIUM];
        }
        else
        {
            physics = data.mModel[LLModel::LOD_HIGH];
        }

        llassert(physics != NULL);

        DecompRequest* request = new DecompRequest(physics, data.mBaseModel, this);
        if(request->isValid())
        {
            gMeshRepo.mDecompThread->submitRequest(request);
            has_valid_requests = true ;
        }
    }

    if (has_valid_requests)
    {
        // *NOTE:  Interesting livelock condition on shutdown.  If there
        // is an upload request in generateHulls() when shutdown starts,
        // the main thread isn't available to manage communication between
        // the decomposition thread and the upload thread and this loop
        // wouldn't complete in turn stalling the main thread.  The check
        // on isDiscarded() prevents that.
        while (! mPhysicsComplete && ! isDiscarded())
        {
            apr_sleep(100);
        }
    }
}

void LLMeshUploadThread::doWholeModelUpload()
{
    LL_DEBUGS(LOG_MESH) << "Starting model upload.  Instances:  " << mInstance.size() << LL_ENDL;

    if (mWholeModelUploadURL.empty())
    {
        LL_WARNS(LOG_MESH) << "Missing mesh upload capability, unable to upload, fee request failed."
                           << LL_ENDL;
    }
    else
    {
        generateHulls();
        LL_DEBUGS(LOG_MESH) << "Hull generation completed." << LL_ENDL;

        mModelData = LLSD::emptyMap();
        wholeModelToLLSD(mModelData, true);
        LLSD body = mModelData["asset_resources"];

        dump_llsd_to_file(body, make_dump_name("whole_model_body_", dump_num));

        LLCore::HttpHandle handle = LLCoreHttpUtil::requestPostWithLLSD(mHttpRequest,
                                                                        mHttpPolicyClass,
                                                                        mWholeModelUploadURL,
                                                                        body,
                                                                        mHttpOptions,
                                                                        mHttpHeaders,
                                                                        LLCore::HttpHandler::ptr_t(this, &NoOpDeletor));
        if (LLCORE_HTTP_HANDLE_INVALID == handle)
        {
            mHttpStatus = mHttpRequest->getStatus();

            LL_WARNS(LOG_MESH) << "Couldn't issue request for full model upload.  Reason:  " << mHttpStatus.toString()
                               << " (" << mHttpStatus.toTerseString() << ")"
                               << LL_ENDL;
        }
        else
        {
            U32 sleep_time(10);

            LL_DEBUGS(LOG_MESH) << "POST request issued." << LL_ENDL;

            mHttpRequest->update(0);
            while (! LLApp::isExiting() && ! finished() && ! isDiscarded())
            {
                ms_sleep(sleep_time);
                sleep_time = llmin(250U, sleep_time + sleep_time);
                mHttpRequest->update(0);
            }

            if (isDiscarded())
            {
                LL_DEBUGS(LOG_MESH) << "Mesh upload operation discarded." << LL_ENDL;
            }
            else
            {
                LL_DEBUGS(LOG_MESH) << "Mesh upload operation completed." << LL_ENDL;
            }
        }
    }
}

void LLMeshUploadThread::requestWholeModelFee()
{
    dump_num++;

    generateHulls();

    mModelData = LLSD::emptyMap();
    wholeModelToLLSD(mModelData, false);
    dump_llsd_to_file(mModelData, make_dump_name("whole_model_fee_request_", dump_num));
    LLCore::HttpHandle handle = LLCoreHttpUtil::requestPostWithLLSD(mHttpRequest,
                                                                    mHttpPolicyClass,
                                                                    mWholeModelFeeCapability,
                                                                    mModelData,
                                                                    mHttpOptions,
                                                                    mHttpHeaders,
                                                                    LLCore::HttpHandler::ptr_t(this, &NoOpDeletor));
    if (LLCORE_HTTP_HANDLE_INVALID == handle)
    {
        mHttpStatus = mHttpRequest->getStatus();

        LL_WARNS(LOG_MESH) << "Couldn't issue request for model fee.  Reason:  " << mHttpStatus.toString()
                           << " (" << mHttpStatus.toTerseString() << ")"
                           << LL_ENDL;
    }
    else
    {
        U32 sleep_time(10);

        mHttpRequest->update(0);
        while (! LLApp::isExiting() && ! finished() && ! isDiscarded())
        {
            ms_sleep(sleep_time);
            sleep_time = llmin(250U, sleep_time + sleep_time);
            mHttpRequest->update(0);
        }
        if (isDiscarded())
        {
            LL_DEBUGS(LOG_MESH) << "Mesh fee query operation discarded." << LL_ENDL;
        }
    }
}


// Does completion duty for both fee queries and actual uploads.
void LLMeshUploadThread::onCompleted(LLCore::HttpHandle handle, LLCore::HttpResponse * response)
{
    // QA/Devel:  0x2 to enable fake error import on upload, 0x1 on fee check
    const S32 fake_error(gSavedSettings.getS32("MeshUploadFakeErrors") & (mDoUpload ? 0xa : 0x5));
    LLCore::HttpStatus status(response->getStatus());
    if (fake_error)
    {
        status = (fake_error & 0x0c) ? LLCore::HttpStatus(500) : LLCore::HttpStatus(200);
    }
    std::string reason(status.toString());
    LLSD body;

    mFinished = true;

    if (mDoUpload)
    {
        // model upload case
        LLWholeModelUploadObserver * observer(mUploadObserverHandle.get());

        if (! status)
        {
            LL_WARNS(LOG_MESH) << "Upload failed.  Reason:  " << reason
                               << " (" << status.toTerseString() << ")"
                               << LL_ENDL;

            // Build a fake body for the alert generator
            body["error"] = LLSD::emptyMap();
            body["error"]["message"] = reason;
            body["error"]["identifier"] = "NetworkError";       // from asset-upload/upload_util.py
            log_upload_error(status, body, "upload", mModelData["name"].asString());

            if (observer)
            {
                doOnIdleOneTime(boost::bind(&LLWholeModelUploadObserver::onModelUploadFailure, observer));
            }
        }
        else
        {
            if (fake_error & 0x2)
            {
                body = llsd_from_file("fake_upload_error.xml");
            }
            else
            {
                // *TODO:  handle error in conversion process
                LLCoreHttpUtil::responseToLLSD(response, true, body);
            }
            dump_llsd_to_file(body, make_dump_name("whole_model_upload_response_", dump_num));

            if (body["state"].asString() == "complete")
            {
                // requested "mesh" asset type isn't actually the type
                // of the resultant object, fix it up here.
                mModelData["asset_type"] = "object";
                gMeshRepo.updateInventory(LLMeshRepository::inventory_data(mModelData, body));

                if (observer)
                {
                    doOnIdleOneTime(boost::bind(&LLWholeModelUploadObserver::onModelUploadSuccess, observer));
                }
            }
            else
            {
                LL_WARNS(LOG_MESH) << "Upload failed.  Not in expected 'complete' state." << LL_ENDL;
                log_upload_error(status, body, "upload", mModelData["name"].asString());

                if (observer)
                {
                    doOnIdleOneTime(boost::bind(&LLWholeModelUploadObserver::onModelUploadFailure, observer));
                }
            }
        }
    }
    else
    {
        // model fee case
        LLWholeModelFeeObserver* observer(mFeeObserverHandle.get());
        mWholeModelUploadURL.clear();

        if (! status)
        {
            LL_WARNS(LOG_MESH) << "Fee request failed.  Reason:  " << reason
                               << " (" << status.toTerseString() << ")"
                               << LL_ENDL;

            // Build a fake body for the alert generator
            body["error"] = LLSD::emptyMap();
            body["error"]["message"] = reason;
            body["error"]["identifier"] = "NetworkError";       // from asset-upload/upload_util.py
            log_upload_error(status, body, "fee", mModelData["name"].asString());

            if (observer)
            {
                observer->setModelPhysicsFeeErrorStatus(status.toULong(), reason, body["error"]);
            }
        }
        else
        {
            if (fake_error & 0x1)
            {
                body = llsd_from_file("fake_upload_error.xml");
            }
            else
            {
                // *TODO:  handle error in conversion process
                LLCoreHttpUtil::responseToLLSD(response, true, body);
            }
            dump_llsd_to_file(body, make_dump_name("whole_model_fee_response_", dump_num));

            if (body["state"].asString() == "upload")
            {
                mWholeModelUploadURL = body["uploader"].asString();

                if (observer)
                {
                    body["data"]["upload_price"] = body["upload_price"];
                    observer->onModelPhysicsFeeReceived(body["data"], mWholeModelUploadURL);
                }
            }
            else
            {
                LL_WARNS(LOG_MESH) << "Fee request failed.  Not in expected 'upload' state." << LL_ENDL;
                log_upload_error(status, body, "fee", mModelData["name"].asString());

                if (observer)
                {
                    observer->setModelPhysicsFeeErrorStatus(status.toULong(), reason, body["error"]);
                }
            }
        }
    }
}


void LLMeshRepoThread::notifyLoadedMeshes()
{
    bool update_metrics(false);

    if (!mMutex)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED;

    if (!mLoadedQ.empty())
    {
        std::deque<LoadedMesh> loaded_queue;

        mLoadedMutex->lock();
        if (!mLoadedQ.empty())
        {
            loaded_queue.swap(mLoadedQ);
            mLoadedMutex->unlock();

            update_metrics = true;

            // Process the elements free of the lock
            for (const auto& mesh : loaded_queue)
            {
                if (mesh.mVolume->getNumVolumeFaces() > 0)
                {
                    gMeshRepo.notifyMeshLoaded(mesh.mMeshParams, mesh.mVolume, mesh.mLOD);
                }
                else
                {
                    gMeshRepo.notifyMeshUnavailable(mesh.mMeshParams, mesh.mLOD, LLVolumeLODGroup::getVolumeDetailFromScale(mesh.mVolume->getDetail()));
                }
            }
        }
        else
        {
            mLoadedMutex->unlock();
        }
    }

    if (!mUnavailableQ.empty())
    {
        std::deque<LODRequest> unavil_queue;

        mLoadedMutex->lock();
        if (!mUnavailableQ.empty())
        {
            unavil_queue.swap(mUnavailableQ);
            mLoadedMutex->unlock();

            update_metrics = true;

            // Process the elements free of the lock
            for (const auto& req : unavil_queue)
            {
                gMeshRepo.notifyMeshUnavailable(req.mMeshParams, req.mLOD, req.mLOD);
            }
        }
        else
        {
            mLoadedMutex->unlock();
        }
    }

    if (!mSkinInfoQ.empty() || !mSkinUnavailableQ.empty() || ! mDecompositionQ.empty())
    {
        if (mLoadedMutex->trylock())
        {
            std::deque<LLPointer<LLMeshSkinInfo>> skin_info_q;
            std::deque<UUIDBasedRequest> skin_info_unavail_q;
            std::list<LLModel::Decomposition*> decomp_q;

            if (! mSkinInfoQ.empty())
            {
                skin_info_q.swap(mSkinInfoQ);
            }

            if (! mSkinUnavailableQ.empty())
            {
                skin_info_unavail_q.swap(mSkinUnavailableQ);
            }

            if (! mDecompositionQ.empty())
            {
                decomp_q.swap(mDecompositionQ);
            }

            mLoadedMutex->unlock();

            // Process the elements free of the lock
            while (! skin_info_q.empty())
            {
                gMeshRepo.notifySkinInfoReceived(skin_info_q.front());
                skin_info_q.pop_front();
            }
            while (! skin_info_unavail_q.empty())
            {
                gMeshRepo.notifySkinInfoUnavailable(skin_info_unavail_q.front().mId);
                skin_info_unavail_q.pop_front();
            }

            while (! decomp_q.empty())
            {
                gMeshRepo.notifyDecompositionReceived(decomp_q.front());
                decomp_q.pop_front();
            }
        }
    }

    if (update_metrics)
    {
        // Ping time-to-load metrics for mesh download operations.
        LLMeshRepository::metricsProgress(0);
    }

}

S32 LLMeshRepoThread::getActualMeshLOD(const LLVolumeParams& mesh_params, S32 lod)
{ //only ever called from main thread
    LLMutexLock lock(mHeaderMutex);
    mesh_header_map::iterator iter = mMeshHeader.find(mesh_params.getSculptID());

    if (iter != mMeshHeader.end())
    {
        auto& header = iter->second;
        if (header.mHeaderSize > 0)
        {
            return LLMeshRepository::getActualMeshLOD(header, lod);
        }
    }

    return lod;
}

//static
S32 LLMeshRepository::getActualMeshLOD(LLMeshHeader& header, S32 lod)
{
    lod = llclamp(lod, 0, 3);

    if (header.m404)
    {
        return -1;
    }

    S32 version = header.mVersion;

    if (version > MAX_MESH_VERSION)
    {
        return -1;
    }

    if (header.mLodSize[lod] > 0)
    {
        return lod;
    }

    //search down to find the next available lower lod
    for (S32 i = lod-1; i >= 0; --i)
    {
        if (header.mLodSize[i] > 0)
        {
            return i;
        }
    }

    //search up to find then ext available higher lod
    for (S32 i = lod+1; i < LLVolumeLODGroup::NUM_LODS; ++i)
    {
        if (header.mLodSize[i] > 0)
        {
            return i;
        }
    }

    //header exists and no good lod found, treat as 404
    header.m404 = true;

    return -1;
}

// Handle failed or successful requests for mesh assets.
//
// Support for 200 responses was added for several reasons.  One,
// a service or cache can ignore range headers and give us a
// 200 with full asset should it elect to.  We also support
// a debug flag which disables range requests for those very
// few users that have some sort of problem with their networking
// services.  But the 200 response handling is suboptimal:  rather
// than cache the whole asset, we just extract the part that would
// have been sent in a 206 and process that.  Inefficient but these
// are cases far off the norm.
void LLMeshHandlerBase::onCompleted(LLCore::HttpHandle handle, LLCore::HttpResponse * response)
{
    LL_PROFILE_ZONE_SCOPED;
    mProcessed = true;

    unsigned int retries(0U);
    response->getRetries(NULL, &retries);
    LLMeshRepository::sHTTPRetryCount += retries;

    LLCore::HttpStatus status(response->getStatus());
    if (! status || MESH_HTTP_RESPONSE_FAILED)
    {
        processFailure(status);
        ++LLMeshRepository::sHTTPErrorCount;
    }
    else
    {
        // From texture fetch code and may apply here:
        //
        // A warning about partial (HTTP 206) data.  Some grid services
        // do *not* return a 'Content-Range' header in the response to
        // Range requests with a 206 status.  We're forced to assume
        // we get what we asked for in these cases until we can fix
        // the services.
        //
        // May also need to deal with 200 status (full asset returned
        // rather than partial) and 416 (request completely unsatisfyable).
        // Always been exposed to these but are less likely here where
        // speculative loads aren't done.
        LLCore::BufferArray * body(response->getBody());
        S32 body_offset(0);
        U8 * data(NULL);
        auto data_size(body ? body->size() : 0);

        if (data_size > 0)
        {
            static const LLCore::HttpStatus par_status(HTTP_PARTIAL_CONTENT);

            unsigned int offset(0), length(0), full_length(0);

            if (par_status == status)
            {
                // 206 case
                response->getRange(&offset, &length, &full_length);
                if (! offset && ! length)
                {
                    // This is the case where we receive a 206 status but
                    // there wasn't a useful Content-Range header in the response.
                    // This could be because it was badly formatted but is more
                    // likely due to capabilities services which scrub headers
                    // from responses.  Assume we got what we asked for...`
                    // length = data_size;
                    offset = mOffset;
                }
            }
            else
            {
                // 200 case, typically
                offset = 0;
            }

            // *DEBUG:  To test validation below
            // offset += 1;

            // Validate that what we think we received is consistent with
            // what we've asked for.  I.e. first byte we wanted lies somewhere
            // in the response.
            if (offset > mOffset
                || (offset + data_size) <= mOffset
                || (mOffset - offset) >= data_size)
            {
                // No overlap with requested range.  Fail request with
                // suitable error.  Shouldn't happen unless server/cache/ISP
                // is doing something awful.
                LL_WARNS(LOG_MESH) << "Mesh response (bytes ["
                                   << offset << ".." << (offset + length - 1)
                                   << "]) didn't overlap with request's origin (bytes ["
                                   << mOffset << ".." << (mOffset + mRequestedBytes - 1)
                                   << "])." << LL_ENDL;
                processFailure(LLCore::HttpStatus(LLCore::HttpStatus::LLCORE, LLCore::HE_INV_CONTENT_RANGE_HDR));
                ++LLMeshRepository::sHTTPErrorCount;
                goto common_exit;
            }

            // *TODO: Try to get rid of data copying and add interfaces
            // that support BufferArray directly.  Introduce a two-phase
            // handler, optional first that takes a body, fallback second
            // that requires a temporary allocation and data copy.
            body_offset = mOffset - offset;
            data = new(std::nothrow) U8[data_size - body_offset];
            if (data)
            {
                body->read(body_offset, (char *) data, data_size - body_offset);
                LLMeshRepository::sBytesReceived += static_cast<U32>(data_size);
            }
            else
            {
                LL_WARNS(LOG_MESH) << "Failed to allocate " << data_size - body_offset << " memory for mesh response" << LL_ENDL;
                processFailure(LLCore::HttpStatus(LLCore::HttpStatus::LLCORE, LLCore::HE_BAD_ALLOC));
            }
        }

        processData(body, body_offset, data, static_cast<S32>(data_size) - body_offset);

        if (mHasDataOwnership)
        {
            delete [] data;
        }
    }

    // Release handler
common_exit:
    gMeshRepo.mThread->mHttpRequestSet.erase(this->shared_from_this());
}


LLMeshHeaderHandler::~LLMeshHeaderHandler()
{
    if (!LLApp::isExiting())
    {
        if (! mProcessed)
        {
            // something went wrong, retry
            LL_WARNS(LOG_MESH) << "Mesh header fetch canceled unexpectedly, retrying." << LL_ENDL;
            LLMeshRepoThread::HeaderRequest req(mMeshParams);
            LLMutexLock lock(gMeshRepo.mThread->mMutex);
            gMeshRepo.mThread->mHeaderReqQ.push(req);
        }
        LLMeshRepoThread::decActiveHeaderRequests();
    }
}

void LLMeshHeaderHandler::processFailure(LLCore::HttpStatus status)
{
    LL_WARNS(LOG_MESH) << "Error during mesh header handling.  ID:  " << mMeshParams.getSculptID()
                       << ", Reason:  " << status.toString()
                       << " (" << status.toTerseString() << ").  Not retrying."
                       << LL_ENDL;

    // Can't get the header so none of the LODs will be available
    LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
    for (int i(0); i < LLVolumeLODGroup::NUM_LODS; ++i)
    {
        gMeshRepo.mThread->mUnavailableQ.push_back(LLMeshRepoThread::LODRequest(mMeshParams, i));
    }
}

void LLMeshHeaderHandler::processData(LLCore::BufferArray * /* body */, S32 /* body_offset */,
                                      U8 * data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    LLUUID mesh_id = mMeshParams.getSculptID();
    bool success = (!MESH_HEADER_PROCESS_FAILED)
        && ((data != NULL) == (data_size > 0)); // if we have data but no size or have size but no data, something is wrong;
    llassert(success);
    EMeshProcessingResult res = MESH_UNKNOWN;
    if (success)
    {
        res = gMeshRepo.mThread->headerReceived(mMeshParams, data, data_size);
        success = (res == MESH_OK);
    }
    if (! success)
    {
        // *TODO:  Get real reason for parse failure here.  Might we want to retry?
        LL_WARNS(LOG_MESH) << "Unable to parse mesh header.  ID:  " << mesh_id
                           << ", Size: " << data_size
                           << ", Reason: " << res << " Not retrying."
                           << LL_ENDL;

        // Can't get the header so none of the LODs will be available
        LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
        for (int i(0); i < LLVolumeLODGroup::NUM_LODS; ++i)
        {
            gMeshRepo.mThread->mUnavailableQ.push_back(LLMeshRepoThread::LODRequest(mMeshParams, i));
        }
    }
    else if (data && data_size > 0)
    {
        // header was successfully retrieved from sim and parsed and is in cache
        S32 header_bytes = 0;
        LLMeshHeader header;

        gMeshRepo.mThread->mHeaderMutex->lock();
        LLMeshRepoThread::mesh_header_map::iterator iter = gMeshRepo.mThread->mMeshHeader.find(mesh_id);
        if (iter != gMeshRepo.mThread->mMeshHeader.end())
        {
            header = iter->second;
            header_bytes = header.mHeaderSize;
        }

        if (header_bytes > 0
            && !header.m404
            && (header.mVersion <= MAX_MESH_VERSION))
        {
            std::stringstream str;

            S32 lod_bytes = 0;

            for (U32 i = 0; i < LLModel::LOD_PHYSICS; ++i)
            {
                // figure out how many bytes we'll need to reserve in the file
                lod_bytes = llmax(lod_bytes, header.mLodOffset[i]+header.mLodSize[i]);
            }

            // just in case skin info or decomposition is at the end of the file (which it shouldn't be)
            lod_bytes = llmax(lod_bytes, header.mSkinOffset+header.mSkinSize);
            lod_bytes = llmax(lod_bytes, header.mPhysicsConvexOffset + header.mPhysicsConvexSize);

            // Do not unlock mutex untill we are done with LLSD.
            // LLSD is smart and can work like smart pointer, is not thread safe.
            gMeshRepo.mThread->mHeaderMutex->unlock();

            S32 bytes = lod_bytes + header_bytes + CACHE_PREAMBLE_SIZE;


            // It's possible for the remote asset to have more data than is needed for the local cache
            // only allocate as much space in the cache as is needed for the local cache
            data_size = llmin(data_size, bytes);

            LLFileSystem file(mesh_id, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);
            if (file.getMaxSize() >= bytes)
            {
                LLMeshRepository::sCacheBytesWritten += data_size;
                ++LLMeshRepository::sCacheWrites;

                // write preamble
                U32 flags = header.getFlags();
                write_preamble(file, header_bytes, flags);

                // write header
                file.write(data, data_size);

                S32 remaining = bytes - file.tell();
                if (remaining > 0)
                {
                    U8* block = new(std::nothrow) U8[remaining];
                    if (block)
                    {
                        memset(block, 0, remaining);
                        file.write(block, remaining);
                        delete[] block;
                    }
                }
            }
        }
        else
        {
            LL_WARNS(LOG_MESH) << "Trying to cache nonexistent mesh, mesh id: " << mesh_id << LL_ENDL;

            gMeshRepo.mThread->mHeaderMutex->unlock();

            // headerReceived() parsed header, but header's data is invalid so none of the LODs will be available
            LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
            for (int i(0); i < LLVolumeLODGroup::NUM_LODS; ++i)
            {
                gMeshRepo.mThread->mUnavailableQ.push_back(LLMeshRepoThread::LODRequest(mMeshParams, i));
            }
        }
    }
}

LLMeshLODHandler::~LLMeshLODHandler()
{
    if (! LLApp::isExiting())
    {
        if (! mProcessed)
        {
            LL_WARNS(LOG_MESH) << "Mesh LOD fetch canceled unexpectedly, retrying." << LL_ENDL;
            gMeshRepo.mThread->lockAndLoadMeshLOD(mMeshParams, mLOD);
        }
        LLMeshRepoThread::decActiveLODRequests();
    }
}

void LLMeshLODHandler::processFailure(LLCore::HttpStatus status)
{
    LL_WARNS(LOG_MESH) << "Error during mesh LOD handling.  ID:  " << mMeshParams.getSculptID()
                       << ", Reason:  " << status.toString()
                       << " (" << status.toTerseString() << ").  Not retrying."
                       << LL_ENDL;

    LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
    gMeshRepo.mThread->mUnavailableQ.push_back(LLMeshRepoThread::LODRequest(mMeshParams, mLOD));
}
void LLMeshLODHandler::processLod(U8* data, S32 data_size)
{
    EMeshProcessingResult result = gMeshRepo.mThread->lodReceived(mMeshParams, mLOD, data, data_size);
    if (result == MESH_OK)
    {
        // good fetch from sim, write to cache
        LLFileSystem file(mMeshParams.getSculptID(), LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);

        S32 offset = mOffset + CACHE_PREAMBLE_SIZE;
        S32 size = mRequestedBytes;

        if (file.getSize() >= offset + size)
        {
            S32 header_bytes = 0;
            U32 flags = 0;
            {
                LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);

                LLMeshRepoThread::mesh_header_map::iterator header_it = gMeshRepo.mThread->mMeshHeader.find(mMeshParams.getSculptID());
                if (header_it != gMeshRepo.mThread->mMeshHeader.end())
                {
                    LLMeshHeader& header = header_it->second;
                    // update header
                    if (!header.mLodInCache[mLOD])
                    {
                        header.mLodInCache[mLOD] = true;
                        header_bytes = header.mHeaderSize;
                        flags = header.getFlags();
                    }
                    // todo: handle else because we shouldn't have requested twice?
                }
            }
            if (flags > 0)
            {
                write_preamble(file, header_bytes, flags);
            }

            file.seek(offset, 0);
            file.write(data, size);
            LLMeshRepository::sCacheBytesWritten += size;
            ++LLMeshRepository::sCacheWrites;
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Error during mesh LOD processing.  ID:  " << mMeshParams.getSculptID()
            << ", Reason: " << result
            << " LOD: " << mLOD
            << " Data size: " << data_size
            << " Not retrying."
            << LL_ENDL;
        LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
        gMeshRepo.mThread->mUnavailableQ.push_back(LLMeshRepoThread::LODRequest(mMeshParams, mLOD));
    }
}

void LLMeshLODHandler::processData(LLCore::BufferArray * /* body */, S32 /* body_offset */,
                                   U8 * data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    if ((!MESH_LOD_PROCESS_FAILED)
        && ((data != NULL) == (data_size > 0))) // if we have data but no size or have size but no data, something is wrong
    {
        LLMeshHandlerBase::ptr_t shrd_handler = shared_from_this();
        bool posted = gMeshRepo.mThread->mMeshThreadPool->getQueue().post(
            [shrd_handler, data, data_size]
            ()
        {
            if (gMeshRepo.mThread->isShuttingDown())
            {
                delete[] data;
                return;
            }
            LLMeshLODHandler* handler = (LLMeshLODHandler * )shrd_handler.get();
            handler->processLod(data, data_size);
            delete[] data;
        });

        if (posted)
        {
            // ownership of data was passed to the lambda
            mHasDataOwnership = false;
        }
        else
        {
            // mesh thread dies later than event queue, so this is normal
            LL_INFOS_ONCE(LOG_MESH) << "Failed to post work into mMeshThreadPool" << LL_ENDL;
            processLod(data, data_size);
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Error during mesh LOD processing.  ID:  " << mMeshParams.getSculptID()
                           << ", Unknown reason.  Not retrying."
                           << " LOD: " << mLOD
                           << " Data size: " << data_size
                           << LL_ENDL;
        LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
        gMeshRepo.mThread->mUnavailableQ.push_back(LLMeshRepoThread::LODRequest(mMeshParams, mLOD));
    }
}

LLMeshSkinInfoHandler::~LLMeshSkinInfoHandler()
{
    if (!mProcessed)
    {
        LL_WARNS(LOG_MESH) << "deleting unprocessed request handler (may be ok on exit)" << LL_ENDL;
    }
    LLMeshRepoThread::decActiveSkinRequests();
}

void LLMeshSkinInfoHandler::processFailure(LLCore::HttpStatus status)
{
    LL_WARNS(LOG_MESH) << "Error during mesh skin info handling.  ID:  " << mMeshID
                       << ", Reason:  " << status.toString()
                       << " (" << status.toTerseString() << ").  Not retrying."
                       << LL_ENDL;
        LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
        gMeshRepo.mThread->mSkinUnavailableQ.emplace_back(mMeshID);
}

void LLMeshSkinInfoHandler::processSkin(U8* data, S32 data_size)
{
    if (gMeshRepo.mThread->skinInfoReceived(mMeshID, data, data_size))
    {
        // good fetch from sim, write to cache
        LLFileSystem file(mMeshID, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);

        S32 offset = mOffset + CACHE_PREAMBLE_SIZE;
        S32 size = mRequestedBytes;

        if (file.getSize() >= offset + size)
        {
            LLMeshRepository::sCacheBytesWritten += size;
            ++LLMeshRepository::sCacheWrites;

            S32 header_bytes = 0;
            U32 flags = 0;
            {
                LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);

                LLMeshRepoThread::mesh_header_map::iterator header_it = gMeshRepo.mThread->mMeshHeader.find(mMeshID);
                if (header_it != gMeshRepo.mThread->mMeshHeader.end())
                {
                    LLMeshHeader& header = header_it->second;
                    // update header
                    if (!header.mSkinInCache)
                    {
                        header.mSkinInCache = true;
                        header_bytes = header.mHeaderSize;
                        flags = header.getFlags();
                    }
                    // todo: handle else because we shouldn't have requested twice?
                }
            }
            if (flags > 0)
            {
                write_preamble(file, header_bytes, flags);
            }

            file.seek(offset, 0);
            file.write(data, size);
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Error during mesh skin info processing.  ID:  " << mMeshID
            << ", Unknown reason.  Not retrying."
            << LL_ENDL;
        LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
        gMeshRepo.mThread->mSkinUnavailableQ.emplace_back(mMeshID);
    }
}

void LLMeshSkinInfoHandler::processData(LLCore::BufferArray * /* body */, S32 /* body_offset */,
                                        U8 * data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    if ((!MESH_SKIN_INFO_PROCESS_FAILED)
        && ((data != NULL) == (data_size > 0))) // if we have data but no size or have size but no data, something is wrong
    {
        LLMeshHandlerBase::ptr_t shrd_handler = shared_from_this();
        bool posted = gMeshRepo.mThread->mMeshThreadPool->getQueue().post(
            [shrd_handler, data, data_size]
            ()
        {
            if (gMeshRepo.mThread->isShuttingDown())
            {
                delete[] data;
                return;
            }
            LLMeshSkinInfoHandler* handler = (LLMeshSkinInfoHandler*)shrd_handler.get();
            handler->processSkin(data, data_size);
            delete[] data;
        });

        if (posted)
        {
            // ownership of data was passed to the lambda
            mHasDataOwnership = false;
        }
        else
        {
            // mesh thread dies later than event queue, so this is normal
            LL_INFOS_ONCE(LOG_MESH) << "Failed to post work into mMeshThreadPool" << LL_ENDL;
            processSkin(data, data_size);
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Error during mesh skin info processing.  ID:  " << mMeshID
                           << ", Unknown reason.  Not retrying."
                           << LL_ENDL;
        LLMutexLock lock(gMeshRepo.mThread->mLoadedMutex);
        gMeshRepo.mThread->mSkinUnavailableQ.emplace_back(mMeshID);
    }
}

LLMeshDecompositionHandler::~LLMeshDecompositionHandler()
{
    if (!mProcessed)
    {
        LL_WARNS(LOG_MESH) << "deleting unprocessed request handler (may be ok on exit)" << LL_ENDL;
    }
}

void LLMeshDecompositionHandler::processFailure(LLCore::HttpStatus status)
{
    LL_WARNS(LOG_MESH) << "Error during mesh decomposition handling.  ID:  " << mMeshID
                       << ", Reason:  " << status.toString()
                       << " (" << status.toTerseString() << ").  Not retrying."
                       << LL_ENDL;
    // *TODO:  Mark mesh unavailable on error.  For now, simply leave
    // request unfulfilled rather than retry forever.
}

void LLMeshDecompositionHandler::processData(LLCore::BufferArray * /* body */, S32 /* body_offset */,
                                             U8 * data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    if ((!MESH_DECOMP_PROCESS_FAILED)
        && ((data != NULL) == (data_size > 0)) // if we have data but no size or have size but no data, something is wrong
        && gMeshRepo.mThread->decompositionReceived(mMeshID, data, data_size))
    {
        // good fetch from sim, write to cache
        LLFileSystem file(mMeshID, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);

        S32 offset = mOffset + CACHE_PREAMBLE_SIZE;
        S32 size = mRequestedBytes;

        if (file.getSize() >= offset+size)
        {
            LLMeshRepository::sCacheBytesWritten += size;
            ++LLMeshRepository::sCacheWrites;

            S32 header_bytes = 0;
            U32 flags = 0;
            {
                LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);

                LLMeshRepoThread::mesh_header_map::iterator header_it = gMeshRepo.mThread->mMeshHeader.find(mMeshID);
                if (header_it != gMeshRepo.mThread->mMeshHeader.end())
                {
                    LLMeshHeader& header = header_it->second;
                    // update header
                    if (!header.mPhysicsConvexInCache)
                    {
                        header.mPhysicsConvexInCache = true;
                        header_bytes = header.mHeaderSize;
                        flags = header.getFlags();
                    }
                    // todo: handle else because we shouldn't have requested twice?
                }
            }
            if (flags > 0)
            {
                write_preamble(file, header_bytes, flags);
            }

            file.seek(offset, 0);
            file.write(data, size);
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Error during mesh decomposition processing.  ID:  " << mMeshID
                           << ", Unknown reason.  Not retrying."
                           << LL_ENDL;
        // *TODO:  Mark mesh unavailable on error
    }
}

LLMeshPhysicsShapeHandler::~LLMeshPhysicsShapeHandler()
{
    if (!mProcessed)
    {
        LL_WARNS(LOG_MESH) << "deleting unprocessed request handler (may be ok on exit)" << LL_ENDL;
    }
}

void LLMeshPhysicsShapeHandler::processFailure(LLCore::HttpStatus status)
{
    LL_WARNS(LOG_MESH) << "Error during mesh physics shape handling.  ID:  " << mMeshID
                       << ", Reason:  " << status.toString()
                       << " (" << status.toTerseString() << ").  Not retrying."
                       << LL_ENDL;
    // *TODO:  Mark mesh unavailable on error
}

void LLMeshPhysicsShapeHandler::processData(LLCore::BufferArray * /* body */, S32 /* body_offset */,
                                            U8 * data, S32 data_size)
{
    LL_PROFILE_ZONE_SCOPED;
    if ((!MESH_PHYS_SHAPE_PROCESS_FAILED)
        && ((data != NULL) == (data_size > 0)) // if we have data but no size or have size but no data, something is wrong
        && gMeshRepo.mThread->physicsShapeReceived(mMeshID, data, data_size) == MESH_OK)
    {
        // good fetch from sim, write to cache for caching
        LLFileSystem file(mMeshID, LLAssetType::AT_MESH, LLFileSystem::READ_WRITE);

        S32 offset = mOffset + CACHE_PREAMBLE_SIZE;
        S32 size = mRequestedBytes;

        if (file.getSize() >= offset+size)
        {
            LLMeshRepository::sCacheBytesWritten += size;
            ++LLMeshRepository::sCacheWrites;

            S32 header_bytes = 0;
            U32 flags = 0;
            {
                LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);

                LLMeshRepoThread::mesh_header_map::iterator header_it = gMeshRepo.mThread->mMeshHeader.find(mMeshID);
                if (header_it != gMeshRepo.mThread->mMeshHeader.end())
                {
                    LLMeshHeader& header = header_it->second;
                    // update header
                    if (!header.mPhysicsMeshInCache)
                    {
                        header.mPhysicsMeshInCache = true;
                        header_bytes = header.mHeaderSize;
                        flags = header.getFlags();
                    }
                    // todo: handle else because we shouldn't have requested twice?
                }
            }
            if (flags > 0)
            {
                write_preamble(file, header_bytes, flags);
            }

            file.seek(offset, 0);
            file.write(data, size);
        }
    }
    else
    {
        LL_WARNS(LOG_MESH) << "Error during mesh physics shape processing.  ID:  " << mMeshID
                           << ", Unknown reason.  Not retrying."
                           << LL_ENDL;
        // *TODO:  Mark mesh unavailable on error
    }
}

LLMeshRepository::LLMeshRepository()
: mMeshMutex(NULL),
  mDecompThread(NULL),
  mMeshThreadCount(0),
  mLegacyGetMeshVersion(0), // <FS:Ansariel> [UDP Assets]
  mThread(NULL)
{
    mSkinInfoCullTimer.resetWithExpiry(10.f);
}

void LLMeshRepository::init()
{
    mMeshMutex = new LLMutex();

    LLConvexDecomposition::getInstance()->initSystem();

    if (!LLConvexDecomposition::isFunctional())
    {
        LL_INFOS(LOG_MESH) << "Using STUB for LLConvexDecomposition" << LL_ENDL;
    }

    mDecompThread = new LLPhysicsDecomp();
    mDecompThread->start();

    while (!mDecompThread->mInited)
    { //wait for physics decomp thread to init
        apr_sleep(100);
    }

    metrics_teleport_started_signal = LLViewerMessage::getInstance()->setTeleportStartedCallback(teleport_started);

    mThread = new LLMeshRepoThread();
    mThread->start();
}

void LLMeshRepository::shutdown()
{
    LL_INFOS(LOG_MESH) << "Shutting down mesh repository." << LL_ENDL;
    llassert(mThread != NULL);
    llassert(mThread->mSignal != NULL);

    metrics_teleport_started_signal.disconnect();

    for (U32 i = 0; i < mUploads.size(); ++i)
    {
        LL_INFOS(LOG_MESH) << "Discard the pending mesh uploads." << LL_ENDL;
        mUploads[i]->discard() ; //discard the uploading requests.
    }

    mThread->cleanup();

    while (!mThread->isStopped())
    {
        apr_sleep(10);
    }
    delete mThread;
    mThread = NULL;

    for (U32 i = 0; i < mUploads.size(); ++i)
    {
        LL_INFOS(LOG_MESH) << "Waiting for pending mesh upload " << (i + 1) << "/" << mUploads.size() << LL_ENDL;
        while (!mUploads[i]->isStopped())
        {
            apr_sleep(10);
        }
        delete mUploads[i];
    }

    mUploads.clear();

    delete mMeshMutex;
    mMeshMutex = NULL;

    LL_INFOS(LOG_MESH) << "Shutting down decomposition system." << LL_ENDL;

    if (mDecompThread)
    {
        mDecompThread->shutdown();
        delete mDecompThread;
        mDecompThread = NULL;
    }

    LLConvexDecomposition::quitSystem();
}

//called in the main thread.
S32 LLMeshRepository::update()
{
    // Conditionally log a mesh metrics event
    metricsUpdate();

    if(mUploadWaitList.empty())
    {
        return 0 ;
    }

    auto size = mUploadWaitList.size() ;
    for (size_t i = 0; i < size; ++i)
    {
        mUploads.push_back(mUploadWaitList[i]);
        mUploadWaitList[i]->preStart() ;
        mUploadWaitList[i]->start() ;
    }
    mUploadWaitList.clear() ;

    return static_cast<S32>(size);
}

void LLMeshRepository::unregisterMesh(LLVOVolume* vobj)
{
    for (auto& lod : mLoadingMeshes)
    {
        for (auto& param : lod)
        {
            vector_replace_with_last(param.second.mVolumes, vobj);
        }
    }

    for (auto& skin_pair : mLoadingSkins)
    {
        vector_replace_with_last(skin_pair.second.mVolumes, vobj);
    }
}

S32 LLMeshRepository::loadMesh(LLVOVolume* vobj, const LLVolumeParams& mesh_params, S32 new_lod, S32 last_lod)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK; //LL_LL_RECORD_BLOCK_TIME(FTM_MESH_FETCH);

    // Manage time-to-load metrics for mesh download operations.
    metricsProgress(1);

    if (new_lod < 0 || new_lod >= LLVolumeLODGroup::NUM_LODS)
    {
        return new_lod;
    }

    {
        LLMutexLock lock(mMeshMutex);
        //add volume to list of loading meshes
        const auto& mesh_id = mesh_params.getSculptID();
        mesh_load_map::iterator iter = mLoadingMeshes[new_lod].find(mesh_id);
        if (iter != mLoadingMeshes[new_lod].end())
        { //request pending for this mesh, append volume id to list
            auto it = std::find(iter->second.mVolumes.begin(), iter->second.mVolumes.end(), vobj);
            if (it == iter->second.mVolumes.end()) {
                iter->second.addVolume(vobj);
            }
        }
        else
        {
            //first request for this mesh
            std::shared_ptr<PendingRequestBase> request(new PendingRequestLOD(mesh_params, new_lod));
            mPendingRequests.emplace_back(request);
            mLoadingMeshes[new_lod][mesh_id].initData(vobj, request);
            LLMeshRepository::sLODPending++;
        }
    }

    //do a quick search to see if we can't display something while we wait for this mesh to load
    LLVolume* volume = vobj->getVolume();

    if (volume)
    {
        LLVolumeParams params = volume->getParams();

        LLVolumeLODGroup* group = LLPrimitive::getVolumeManager()->getGroup(params);

        if (group)
        {
            //first, see if last_lod is available (don't transition down to avoid funny popping a la SH-641)
            if (last_lod >= 0)
            {
                LLVolume* lod = group->refLOD(last_lod);
                if (lod && lod->isMeshAssetLoaded() && lod->getNumVolumeFaces() > 0)
                {
                    group->derefLOD(lod);
                    return last_lod;
                }
                group->derefLOD(lod);
            }

            //next, see what the next lowest LOD available might be
            for (S32 i = new_lod -1; i >= 0; --i)
            {
                LLVolume* lod = group->refLOD(i);
                if (lod && lod->isMeshAssetLoaded() && lod->getNumVolumeFaces() > 0)
                {
                    group->derefLOD(lod);
                    return i;
                }

                group->derefLOD(lod);
            }

            //no lower LOD is a available, is a higher lod available?
            for (S32 i = new_lod+1; i < LLVolumeLODGroup::NUM_LODS; ++i)
            {
                LLVolume* lod = group->refLOD(i);
                if (lod && lod->isMeshAssetLoaded() && lod->getNumVolumeFaces() > 0)
                {
                    group->derefLOD(lod);
                    return i;
                }

                group->derefLOD(lod);
            }
        }
    }

    return new_lod;
}

void LLMeshRepository::notifyLoadedMeshes()
{ //called from main thread
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK; //LL_RECORD_BLOCK_TIME(FTM_MESH_FETCH);

    // <FS:Ansariel> [UDP Assets]
    //// GetMesh2 operation with keepalives, etc.  With pipelining,
    //// we'll increase this.  See llappcorehttp and llcorehttp for
    //// discussion on connection strategies.
    //LLAppCoreHttp & app_core_http(LLAppViewer::instance()->getAppCoreHttp());
    //S32 scale(app_core_http.isPipelined(LLAppCoreHttp::AP_MESH2)
    //          ? (2 * LLAppCoreHttp::PIPELINING_DEPTH)
    //          : 5);

    //static LLCachedControl<U32> mesh2_max_req(gSavedSettings, "Mesh2MaxConcurrentRequests");
    //LLMeshRepoThread::sMaxConcurrentRequests = mesh2_max_req;
    //LLMeshRepoThread::sRequestHighWater = llclamp(scale * S32(LLMeshRepoThread::sMaxConcurrentRequests),
    //                                              REQUEST2_HIGH_WATER_MIN,
    //                                              REQUEST2_HIGH_WATER_MAX);
    //LLMeshRepoThread::sRequestLowWater = llclamp(LLMeshRepoThread::sRequestHighWater / 2,
    //                                             REQUEST2_LOW_WATER_MIN,
    //                                             REQUEST2_LOW_WATER_MAX);

    if (mLegacyGetMeshVersion == 1)
    {
        // Legacy GetMesh operation with high connection concurrency
        static LLCachedControl<U32> meshMaxConcurrentRequests(gSavedSettings, "MeshMaxConcurrentRequests");
        if (meshMaxConcurrentRequests() > MESH_CONCURRENT_REQUEST_LIMIT)
        {
            U32 mesh_max_concurrent_requests_default = gSavedSettings.getControl("MeshMaxConcurrentRequests")->getDefault().asInteger();
            LLSD args;
            args["VALUE"] = llformat("%d", meshMaxConcurrentRequests());
            args["MAX"] = llformat("%d", MESH_CONCURRENT_REQUEST_LIMIT);
            args["DEFAULT"] = llformat("%d", mesh_max_concurrent_requests_default);
            args["DEBUGNAME"] = "MeshMaxConccurrentRequests";
            LLNotificationsUtil::add("MeshMaxConcurrentReqTooHigh", args);
            gSavedSettings.setU32("MeshMaxConcurrentRequests", mesh_max_concurrent_requests_default);
        }
        LLMeshRepoThread::sMaxConcurrentRequests = meshMaxConcurrentRequests();
        LLMeshRepoThread::sRequestHighWater = llclamp(2 * S32(LLMeshRepoThread::sMaxConcurrentRequests),
                                                      REQUEST_HIGH_WATER_MIN,
                                                      REQUEST_HIGH_WATER_MAX);
        LLMeshRepoThread::sRequestLowWater = llclamp(LLMeshRepoThread::sRequestHighWater / 2,
                                                     REQUEST_LOW_WATER_MIN,
                                                     REQUEST_LOW_WATER_MAX);
    }
    else
    {
        // GetMesh2 operation with keepalives, etc.  With pipelining,
        // we'll increase this.  See llappcorehttp and llcorehttp for
        // discussion on connection strategies.
        LLAppCoreHttp & app_core_http(LLAppViewer::instance()->getAppCoreHttp());
        S32 scale(app_core_http.isPipelined(LLAppCoreHttp::AP_MESH2)
                  ? (2 * LLAppCoreHttp::PIPELINING_DEPTH)
                  : 5);

        static LLCachedControl<U32> mesh2MaxConcurrentRequests(gSavedSettings, "Mesh2MaxConcurrentRequests");
        if (mesh2MaxConcurrentRequests() > MESH2_CONCURRENT_REQUEST_LIMIT)
        {
            U32 mesh2_max_concurrent_requests_default = gSavedSettings.getControl("Mesh2MaxConcurrentRequests")->getDefault().asInteger();
            LLSD args;
            args["VALUE"] = llformat("%d", mesh2MaxConcurrentRequests());
            args["MAX"] = llformat("%d", MESH2_CONCURRENT_REQUEST_LIMIT);
            args["DEFAULT"] = llformat("%d", mesh2_max_concurrent_requests_default);
            args["DEBUGNAME"] = "Mesh2MaxConccurrentRequests";
            LLNotificationsUtil::add("MeshMaxConcurrentReqTooHigh", args);
            gSavedSettings.setU32("Mesh2MaxConcurrentRequests", mesh2_max_concurrent_requests_default);
        }
        LLMeshRepoThread::sMaxConcurrentRequests = mesh2MaxConcurrentRequests();
        LLMeshRepoThread::sRequestHighWater = llclamp(scale * S32(LLMeshRepoThread::sMaxConcurrentRequests),
                                                      REQUEST2_HIGH_WATER_MIN,
                                                      REQUEST2_HIGH_WATER_MAX);
        LLMeshRepoThread::sRequestLowWater = llclamp(LLMeshRepoThread::sRequestHighWater / 2,
                                                     REQUEST2_LOW_WATER_MIN,
                                                     REQUEST2_LOW_WATER_MAX);
    }
    // </FS:Ansariel> [UDP Assets]

    //clean up completed upload threads
    for (std::vector<LLMeshUploadThread*>::iterator iter = mUploads.begin(); iter != mUploads.end(); )
    {
        LLMeshUploadThread* thread = *iter;

        if (thread->isStopped() && thread->finished())
        {
            iter = mUploads.erase(iter);
            delete thread;
        }
        else
        {
            ++iter;
        }
    }

    //update inventory
    if (!mInventoryQ.empty())
    {
        LLMutexLock lock(mMeshMutex);
        while (!mInventoryQ.empty())
        {
            inventory_data& data = mInventoryQ.front();

            LLAssetType::EType asset_type = LLAssetType::lookup(data.mPostData["asset_type"].asString());
            LLInventoryType::EType inventory_type = LLInventoryType::lookup(data.mPostData["inventory_type"].asString());

            // Handle addition of texture, if any.
            if ( data.mResponse.has("new_texture_folder_id") )
            {
                const LLUUID& new_folder_id = data.mResponse["new_texture_folder_id"].asUUID();

                if ( new_folder_id.notNull() )
                {
                    LLUUID parent_id = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_TEXTURE);

                    std::string name;
                    // Check if the server built a different name for the texture folder
                    if ( data.mResponse.has("new_texture_folder_name") )
                    {
                        name = data.mResponse["new_texture_folder_name"].asString();
                    }
                    else
                    {
                        name = data.mPostData["name"].asString();
                    }

                    // Add the category to the internal representation
                    LLPointer<LLViewerInventoryCategory> cat =
                        new LLViewerInventoryCategory(new_folder_id, parent_id,
                            LLFolderType::FT_NONE, name, gAgent.getID());
                    cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);

                    LLInventoryModel::LLCategoryUpdate update(cat->getParentUUID(), 1);
                    gInventory.accountForUpdate(update);
                    gInventory.updateCategory(cat);
                }
            }

            on_new_single_inventory_upload_complete(
                asset_type,
                inventory_type,
                data.mPostData["asset_type"].asString(),
                data.mPostData["folder_id"].asUUID(),
                data.mPostData["name"],
                data.mPostData["description"],
                data.mResponse,
                data.mResponse["upload_price"]);
            //}

            mInventoryQ.pop();
        }
    }

    //call completed callbacks on finished decompositions
    mDecompThread->notifyCompleted();

    if (mSkinInfoCullTimer.checkExpirationAndReset(10.f))
    {
        //// Clean up dead skin info
        //U64Bytes skinbytes(0);
        for (auto iter = mSkinMap.begin(), ender = mSkinMap.end(); iter != ender;)
        {
            auto copy_iter = iter++;
            LLUUID id = copy_iter->first;

            //skinbytes += U64Bytes(sizeof(LLMeshSkinInfo));
            //skinbytes += U64Bytes(copy_iter->second->mJointNames.size() * sizeof(std::string));
            //skinbytes += U64Bytes(copy_iter->second->mJointNums.size() * sizeof(S32));
            //skinbytes += U64Bytes(copy_iter->second->mJointNames.size() * sizeof(LLMatrix4a));
            //skinbytes += U64Bytes(copy_iter->second->mJointNames.size() * sizeof(LLMatrix4));

            if (copy_iter->second->getNumRefs() == 1)
            {
                mSkinMap.erase(copy_iter);
            }

            // erase from background thread
            mThread->mWorkQueue.post([=, this]()
                {
                    LLMutexLock(mThread->mSkinMapMutex);
                    mThread->mSkinMap.erase(id);
                });
        }
        //LL_INFOS() << "Skin info cache elements:" << mSkinMap.size() << " Memory: " << U64Kilobytes(skinbytes) << LL_ENDL;
    }

    // For major operations, attempt to get the required locks
    // without blocking and punt if they're not available.  The
    // longest run of holdoffs is kept in sMaxLockHoldoffs just
    // to collect the data.  In testing, I've never seen a value
    // greater than 2 (written to log on exit).
    {
        LLMutexTrylock lock1(mMeshMutex);
        LLMutexTrylock lock2(mThread->mMutex);
        LLMutexTrylock lock3(mThread->mHeaderMutex);
        LLMutexTrylock lock4(mThread->mPendingMutex);

        static U32 hold_offs(0);
        if (! lock1.isLocked() || ! lock2.isLocked() || ! lock3.isLocked() || ! lock4.isLocked())
        {
            // If we can't get the locks, skip and pick this up later.
            // Eventually thread queue will be free enough
            ++hold_offs;
            sMaxLockHoldoffs = llmax(sMaxLockHoldoffs, hold_offs);
            if (hold_offs > 4)
            {
                LL_WARNS_ONCE() << "High mesh thread holdoff" << LL_ENDL;
            }
            return;
        }
        hold_offs = 0;

        if (gAgent.getRegion())
        {
            // Update capability urls
            static std::string region_name("never name a region this");

            if (gAgent.getRegion()->getName() != region_name && gAgent.getRegion()->capabilitiesReceived())
            {
                region_name = gAgent.getRegion()->getName();
                // <FS:Ansariel> [UDP Assets]
                //const std::string mesh_cap(gAgent.getRegion()->getViewerAssetUrl());
                //mThread->setGetMeshCap(mesh_cap);
                //LL_DEBUGS(LOG_MESH) << "Retrieving caps for region '" << region_name
                //                  << "', ViewerAsset cap:  " << mesh_cap
                //                  << LL_ENDL;
                const bool use_v1(gSavedSettings.getBOOL("MeshUseGetMesh1"));
                const std::string mesh_cap(gAgent.getRegion()->getViewerAssetUrl());
                const std::string legacy_mesh1_cap(gAgent.getRegion()->getCapability("GetMesh"));
                const std::string legacy_mesh2_cap(gAgent.getRegion()->getCapability("GetMesh2"));
                mLegacyGetMeshVersion = ((mesh_cap.empty() && legacy_mesh2_cap.empty()) || use_v1) ? 1 : (!mesh_cap.empty() ? 0 : 2);
                mThread->setGetMeshCap(mesh_cap, legacy_mesh1_cap, legacy_mesh2_cap, mLegacyGetMeshVersion);
                LL_DEBUGS(LOG_MESH) << "Retrieving caps for region '" << region_name
                                    << "', ViewerAsset cap:  " << mesh_cap
                                    << ", GetMesh2 cap:  " << legacy_mesh2_cap
                                    << ", GetMesh cap:  " << legacy_mesh1_cap
                                    << ", using version:  " << mLegacyGetMeshVersion
                                    << LL_ENDL;
                // <FS:Ansariel> [UDP Assets]
            }
        }

        //popup queued error messages from background threads
        while (!mUploadErrorQ.empty())
        {
            LLSD substitutions(mUploadErrorQ.front());
            if (substitutions.has("DETAILS"))
            {
                LLNotificationsUtil::add("MeshUploadErrorDetails", substitutions);
            }
            else
            {
                LLNotificationsUtil::add("MeshUploadError", substitutions);
            }
            mUploadErrorQ.pop();
        }

        // mPendingRequests go into queues, queues go into active http requests.
        // Checking sRequestHighWater to keep queues at least somewhat populated
        // for faster transition into http
        S32 active_count = LLMeshRepoThread::sActiveHeaderRequests + LLMeshRepoThread::sActiveLODRequests + LLMeshRepoThread::sActiveSkinRequests;
        active_count += (S32)(mThread->mLODReqQ.size() + mThread->mHeaderReqQ.size() + mThread->mSkinInfoQ.size());
        if (active_count < LLMeshRepoThread::sRequestHighWater)
        {
            S32 push_count = LLMeshRepoThread::sRequestHighWater - active_count;

            if (mPendingRequests.size() > push_count)
            {
                LL_PROFILE_ZONE_NAMED("Mesh score update");
                // More requests than the high-water limit allows so
                // sort and forward the most important.

                // update "score" for pending requests
                for (std::shared_ptr<PendingRequestBase>& req_p : mPendingRequests)
                {
                    req_p->checkScore();
                }

                //sort by "score"
                std::partial_sort(mPendingRequests.begin(), mPendingRequests.begin() + push_count,
                                  mPendingRequests.end(), PendingRequestBase::CompareScoreGreater());
            }
            while (!mPendingRequests.empty() && push_count > 0)
            {
                std::shared_ptr<PendingRequestBase>& req_p = mPendingRequests.front();
                // todo: check hasTrackedData here and erase request if none
                // since this is supposed to mean that request was removed
                switch (req_p->getRequestType())
                {
                case MESH_REQUEST_LOD:
                    {
                        PendingRequestLOD* lod = (PendingRequestLOD*)req_p.get();
                        mThread->loadMeshLOD(lod->mMeshParams, lod->mLOD);
                        LLMeshRepository::sLODPending--;
                        break;
                    }
                case MESH_REQUEST_SKIN:
                    {
                        PendingRequestUUID* skin = (PendingRequestUUID*)req_p.get();
                        mThread->loadMeshSkinInfo(skin->getId());
                        break;
                    }

                default:
                    LL_ERRS() << "Unknown request type in LLMeshRepository::notifyLoadedMeshes" << LL_ENDL;
                    break;
                }
                mPendingRequests.erase(mPendingRequests.begin());
                push_count--;
            }
        }

        //send decomposition requests
        while (!mPendingDecompositionRequests.empty())
        {
            mThread->loadMeshDecomposition(mPendingDecompositionRequests.front());
            mPendingDecompositionRequests.pop();
        }

        //send physics shapes decomposition requests
        while (!mPendingPhysicsShapeRequests.empty())
        {
            mThread->loadMeshPhysicsShape(mPendingPhysicsShapeRequests.front());
            mPendingPhysicsShapeRequests.pop();
        }

        mThread->notifyLoadedMeshes();
    }

    mThread->mSignal->signal();
}

void LLMeshRepository::notifySkinInfoReceived(LLMeshSkinInfo* info)
{
    mSkinMap[info->mMeshID] = info; // Cache into LLPointer
    // Alternative: We can get skin size from header
    sCacheBytesSkins += info->sizeBytes();

    skin_load_map::iterator iter = mLoadingSkins.find(info->mMeshID);
    if (iter != mLoadingSkins.end())
    {
        for (LLVOVolume* vobj : iter->second.mVolumes)
        {
            if (vobj)
            {
                vobj->notifySkinInfoLoaded(info);
            }
        }
        mLoadingSkins.erase(iter);
    }
}

void LLMeshRepository::notifySkinInfoUnavailable(const LLUUID& mesh_id)
{
    skin_load_map::iterator iter = mLoadingSkins.find(mesh_id);
    if (iter != mLoadingSkins.end())
    {
        for (LLVOVolume* vobj : iter->second.mVolumes)
        {
            if (vobj)
            {
                vobj->notifySkinInfoUnavailable();
            }
        }
        mLoadingSkins.erase(iter);
    }
}

void LLMeshRepository::notifyDecompositionReceived(LLModel::Decomposition* decomp)
{
    decomposition_map::iterator iter = mDecompositionMap.find(decomp->mMeshID);
    if (iter == mDecompositionMap.end())
    { //just insert decomp into map
        mDecompositionMap[decomp->mMeshID] = decomp;
        mLoadingDecompositions.erase(decomp->mMeshID);
        sCacheBytesDecomps += decomp->sizeBytes();
    }
    else
    { //merge decomp with existing entry
        sCacheBytesDecomps -= iter->second->sizeBytes();
        iter->second->merge(decomp);
        sCacheBytesDecomps += iter->second->sizeBytes();

        mLoadingDecompositions.erase(decomp->mMeshID);
        delete decomp;
    }
}

void LLMeshRepository::notifyMeshLoaded(const LLVolumeParams& mesh_params, LLVolume* volume, S32 lod)
{ //called from main thread

    //get list of objects waiting to be notified this mesh is loaded
    const auto& mesh_id = mesh_params.getSculptID();
    mesh_load_map::iterator obj_iter = mLoadingMeshes[lod].find(mesh_id);

    if (volume && obj_iter != mLoadingMeshes[lod].end())
    {
        //make sure target volume is still valid
        if (volume->getNumVolumeFaces() <= 0)
        {
            LL_WARNS(LOG_MESH) << "Mesh loading returned empty volume.  ID:  " << mesh_id
                               << LL_ENDL;
        }

        { //update system volume
            S32 detail = LLVolumeLODGroup::getVolumeDetailFromScale(volume->getDetail());
            LLVolume* sys_volume = LLPrimitive::getVolumeManager()->refVolume(mesh_params, detail);
            if (sys_volume)
            {
                sys_volume->copyVolumeFaces(volume);
                sys_volume->setMeshAssetLoaded(true);
                LLPrimitive::getVolumeManager()->unrefVolume(sys_volume);
            }
            else
            {
                LL_WARNS(LOG_MESH) << "Couldn't find system volume for mesh " << mesh_id
                                   << LL_ENDL;
            }
        }

        //notify waiting LLVOVolume instances that their requested mesh is available
        for (LLVOVolume* vobj : obj_iter->second.mVolumes)
        {
            if (vobj)
            {
                vobj->notifyMeshLoaded();
            }
        }

        mLoadingMeshes[lod].erase(obj_iter);

        LLViewerStatsRecorder::instance().meshLoaded();
    }
}

void LLMeshRepository::notifyMeshUnavailable(const LLVolumeParams& mesh_params, S32 request_lod, S32 volume_lod)
{ //called from main thread
    //get list of objects waiting to be notified this mesh is loaded
    const auto& mesh_id = mesh_params.getSculptID();
    mesh_load_map::iterator obj_iter = mLoadingMeshes[request_lod].find(mesh_id);
    if (obj_iter != mLoadingMeshes[request_lod].end())
    {
        F32 detail = LLVolumeLODGroup::getVolumeScaleFromDetail(volume_lod);

        LLVolume* sys_volume = LLPrimitive::getVolumeManager()->refVolume(mesh_params, volume_lod);
        if (sys_volume)
        {
            sys_volume->setMeshAssetUnavaliable(true);
            LLPrimitive::getVolumeManager()->unrefVolume(sys_volume);
        }

        for (LLVOVolume* vobj : obj_iter->second.mVolumes)
        {
            if (vobj)
            {
                LLVolume* obj_volume = vobj->getVolume();

                if (obj_volume &&
                    obj_volume->getDetail() == detail &&
                    obj_volume->getParams() == mesh_params)
                { //should force volume to find most appropriate LOD
                    vobj->setVolume(obj_volume->getParams(), volume_lod);
                }
            }
        }

        mLoadingMeshes[request_lod].erase(obj_iter);
    }
}

S32 LLMeshRepository::getActualMeshLOD(const LLVolumeParams& mesh_params, S32 lod)
{
    return mThread->getActualMeshLOD(mesh_params, lod);
}

const LLMeshSkinInfo* LLMeshRepository::getSkinInfo(const LLUUID& mesh_id, LLVOVolume* requesting_obj)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_AVATAR;
    if (mesh_id.notNull())
    {
        skin_map::iterator iter = mSkinMap.find(mesh_id);
        if (iter != mSkinMap.end())
        {
            return iter->second;
        }

        //no skin info known about given mesh, try to fetch it
        if (requesting_obj != nullptr)
        {
            LLMutexLock lock(mMeshMutex);
            //add volume to list of loading meshes
            skin_load_map::iterator iter = mLoadingSkins.find(mesh_id);
            if (iter != mLoadingSkins.end())
            { //request pending for this mesh, append volume id to list
                auto it = std::find(iter->second.mVolumes.begin(), iter->second.mVolumes.end(), requesting_obj);
                if (it == iter->second.mVolumes.end()) {
                    iter->second.addVolume(requesting_obj);
                }
            }
            else
            {
                //first request for this mesh
                std::shared_ptr<PendingRequestBase> request(new PendingRequestUUID(mesh_id, MESH_REQUEST_SKIN));
                mLoadingSkins[mesh_id].initData(requesting_obj, request);
                mPendingRequests.emplace_back(request);
            }
        }
    }
    return nullptr;
}

void LLMeshRepository::fetchPhysicsShape(const LLUUID& mesh_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK; //LL_RECORD_BLOCK_TIME(FTM_MESH_FETCH);

    if (mesh_id.notNull())
    {
        LLModel::Decomposition* decomp = NULL;
        decomposition_map::iterator iter = mDecompositionMap.find(mesh_id);
        if (iter != mDecompositionMap.end())
        {
            decomp = iter->second;
        }

        //decomposition block hasn't been fetched yet
        if (!decomp || decomp->mPhysicsShapeMesh.empty())
        {
            LLMutexLock lock(mMeshMutex);
            //add volume to list of loading meshes
            std::unordered_set<LLUUID>::iterator iter = mLoadingPhysicsShapes.find(mesh_id);
            if (iter == mLoadingPhysicsShapes.end())
            { //no request pending for this skin info
                // *FIXME:  Nothing ever deletes entries, can't be right
                mLoadingPhysicsShapes.insert(mesh_id);
                mPendingPhysicsShapeRequests.push(mesh_id);
            }
        }
    }
}

LLModel::Decomposition* LLMeshRepository::getDecomposition(const LLUUID& mesh_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK; //LL_RECORD_BLOCK_TIME(FTM_MESH_FETCH);

    LLModel::Decomposition* ret = NULL;

    if (mesh_id.notNull())
    {
        decomposition_map::iterator iter = mDecompositionMap.find(mesh_id);
        if (iter != mDecompositionMap.end())
        {
            ret = iter->second;
        }

        //decomposition block hasn't been fetched yet
        if (!ret || ret->mBaseHullMesh.empty())
        {
            LLMutexLock lock(mMeshMutex);
            //add volume to list of loading meshes
            std::unordered_set<LLUUID>::iterator iter = mLoadingDecompositions.find(mesh_id);
            if (iter == mLoadingDecompositions.end())
            { //no request pending for this skin info
                mLoadingDecompositions.insert(mesh_id);
                mPendingDecompositionRequests.push(mesh_id);
            }
        }
    }

    return ret;
}

void LLMeshRepository::buildHull(const LLVolumeParams& params, S32 detail)
{
    LLVolume* volume = LLPrimitive::sVolumeManager->refVolume(params, detail);

    if (!volume->mHullPoints)
    {
        //all default params
        //execute first stage
        //set simplify mode to retain
        //set retain percentage to zero
        //run second stage
    }

    LLPrimitive::sVolumeManager->unrefVolume(volume);
}

bool LLMeshRepository::hasPhysicsShape(const LLUUID& mesh_id)
{
    if (mesh_id.isNull())
    {
        return false;
    }

    if (mThread->hasPhysicsShapeInHeader(mesh_id))
    {
        return true;
    }

    LLModel::Decomposition* decomp = getDecomposition(mesh_id);
    if (decomp && !decomp->mHull.empty())
    {
        return true;
    }

    return false;
}

bool LLMeshRepository::hasSkinInfo(const LLUUID& mesh_id)
{
    LL_PROFILE_ZONE_SCOPED;

    if (mesh_id.isNull())
    {
        return false;
    }

    if (mThread->hasSkinInfoInHeader(mesh_id))
    {
        return true;
    }

    const LLMeshSkinInfo* skininfo = getSkinInfo(mesh_id);
    if (skininfo)
    {
        return true;
    }

    return false;
}

bool LLMeshRepository::hasHeader(const LLUUID& mesh_id) const
{
    if (mesh_id.isNull())
    {
        return false;
    }

    return mThread->hasHeader(mesh_id);
}

bool LLMeshRepoThread::hasPhysicsShapeInHeader(const LLUUID& mesh_id) const
{
    LLMutexLock lock(mHeaderMutex);
    mesh_header_map::const_iterator iter = mMeshHeader.find(mesh_id);
    if (iter != mMeshHeader.end() && iter->second.mHeaderSize > 0)
    {
        const LLMeshHeader &mesh = iter->second;
        if (mesh.mPhysicsMeshSize > 0)
        {
            return true;
        }
    }

    return false;
}

bool LLMeshRepoThread::hasSkinInfoInHeader(const LLUUID& mesh_id) const
{
    LLMutexLock lock(mHeaderMutex);
    mesh_header_map::const_iterator iter = mMeshHeader.find(mesh_id);
    if (iter != mMeshHeader.end() && iter->second.mHeaderSize > 0)
    {
        const LLMeshHeader& mesh = iter->second;
        if (mesh.mSkinOffset >= 0
            && mesh.mSkinSize > 0)
        {
            return true;
        }
    }

    return false;
}

bool LLMeshRepoThread::hasHeader(const LLUUID& mesh_id) const
{
    LLMutexLock lock(mHeaderMutex);
    mesh_header_map::const_iterator iter = mMeshHeader.find(mesh_id);
    return iter != mMeshHeader.end();
}

// <FS:Ansariel> DAE export
LLUUID LLMeshRepository::getCreatorFromHeader(const LLUUID& mesh_id)
{
    if (mesh_id.isNull())
    {
        return LLUUID();
    }

    return mThread->getCreatorFromHeader(mesh_id);
}

LLUUID LLMeshRepoThread::getCreatorFromHeader(const LLUUID& mesh_id)
{
    LLMutexLock lock(mHeaderMutex);
    mesh_header_map::iterator iter = mMeshHeader.find(mesh_id);
    if (iter != mMeshHeader.end() && iter->second.mHeaderSize > 0)
    {
        LLMeshHeader& mesh = iter->second;
        return mesh.mCreatorId;
    }

    return LLUUID();
}
// </FS:Ansariel>

void LLMeshRepository::uploadModel(std::vector<LLModelInstance>& data, LLVector3& scale, bool upload_textures,
                                   bool upload_skin, bool upload_joints, bool lock_scale_if_joint_position,
                                   std::string upload_url, bool do_upload,
                                   LLHandle<LLWholeModelFeeObserver> fee_observer, LLHandle<LLWholeModelUploadObserver> upload_observer)
{
    LLMeshUploadThread* thread = new LLMeshUploadThread(data, scale, upload_textures,
                                                        upload_skin, upload_joints, lock_scale_if_joint_position,
                                                        upload_url, do_upload, fee_observer, upload_observer);
    mUploadWaitList.push_back(thread);
}

S32 LLMeshRepository::getMeshSize(const LLUUID& mesh_id, S32 lod) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    if (mThread && mesh_id.notNull() && LLPrimitive::NO_LOD != lod)
    {
        LLMutexLock lock(mThread->mHeaderMutex);
        LLMeshRepoThread::mesh_header_map::const_iterator iter = mThread->mMeshHeader.find(mesh_id);
        if (iter != mThread->mMeshHeader.end() && iter->second.mHeaderSize > 0)
        {
            const LLMeshHeader& header = iter->second;

            if (header.m404)
            {
                return -1;
            }

            S32 size = header.mLodSize[lod];
            return size;
        }

    }

    return -1;
}

void LLMeshUploadThread::decomposeMeshMatrix(LLMatrix4& transformation,
                                             LLVector3& result_pos,
                                             LLQuaternion& result_rot,
                                             LLVector3& result_scale)
{
    // check for reflection
    bool reflected = (transformation.determinant() < 0);

    // compute position
    LLVector3 position = LLVector3(0, 0, 0) * transformation;

    // compute scale
    LLVector3 x_transformed = LLVector3(1, 0, 0) * transformation - position;
    LLVector3 y_transformed = LLVector3(0, 1, 0) * transformation - position;
    LLVector3 z_transformed = LLVector3(0, 0, 1) * transformation - position;
    F32 x_length = x_transformed.normalize();
    F32 y_length = y_transformed.normalize();
    F32 z_length = z_transformed.normalize();
    LLVector3 scale = LLVector3(x_length, y_length, z_length);

    // adjust for "reflected" geometry
    LLVector3 x_transformed_reflected = x_transformed;
    if (reflected)
    {
        x_transformed_reflected *= -1.0;
    }

    // compute rotation
    LLMatrix3 rotation_matrix;
    rotation_matrix.setRows(x_transformed_reflected, y_transformed, z_transformed);
    LLQuaternion quat_rotation = rotation_matrix.quaternion();
    quat_rotation.normalize(); // the rotation_matrix might not have been orthoginal.  make it so here.
    LLVector3 euler_rotation;
    quat_rotation.getEulerAngles(&euler_rotation.mV[VX], &euler_rotation.mV[VY], &euler_rotation.mV[VZ]);

    result_pos = position + mOrigin;
    result_scale = scale;
    result_rot = quat_rotation;
}

void LLMeshRepository::updateInventory(inventory_data data)
{
    LLMutexLock lock(mMeshMutex);
    dump_llsd_to_file(data.mPostData,make_dump_name("update_inventory_post_data_",dump_num));
    dump_llsd_to_file(data.mResponse,make_dump_name("update_inventory_response_",dump_num));
    mInventoryQ.push(data);
}

void LLMeshRepository::uploadError(LLSD& args)
{
    LLMutexLock lock(mMeshMutex);
    mUploadErrorQ.push(args);
}

F32 LLMeshRepository::getEstTrianglesMax(LLUUID mesh_id)
{
    LLMeshCostData costs;
    if (getCostData(mesh_id, costs))
    {
        return costs.getEstTrisMax();
    }
    else
    {
        return 0.f;
    }
}

F32 LLMeshRepository::getEstTrianglesStreamingCost(LLUUID mesh_id)
{
    LLMeshCostData costs;
    if (getCostData(mesh_id, costs))
    {
        return costs.getEstTrisForStreamingCost();
    }
    else
    {
        return 0.f;
    }
}

// FIXME replace with calc based on LLMeshCostData
F32 LLMeshRepository::getStreamingCostLegacy(LLUUID mesh_id, F32 radius, S32* bytes, S32* bytes_visible, S32 lod, F32 *unscaled_value)
{
    F32 result = 0.f;
    if (mThread && mesh_id.notNull())
    {
        LLMutexLock lock(mThread->mHeaderMutex);
        LLMeshRepoThread::mesh_header_map::iterator iter = mThread->mMeshHeader.find(mesh_id);
        if (iter != mThread->mMeshHeader.end() && iter->second.mHeaderSize > 0)
        {
            result  = getStreamingCostLegacy(iter->second, radius, bytes, bytes_visible, lod, unscaled_value);
        }
    }
    if (result > 0.f)
    {
        LLMeshCostData data;
        if (getCostData(mesh_id, data))
        {
            F32 ref_streaming_cost = data.getRadiusBasedStreamingCost(radius);
            F32 ref_weighted_tris = data.getRadiusWeightedTris(radius);
            if (!is_approx_equal(ref_streaming_cost,result))
            {
                LL_WARNS() << mesh_id << "streaming mismatch " << result << " " << ref_streaming_cost << LL_ENDL;
            }
            if (unscaled_value && !is_approx_equal(ref_weighted_tris,*unscaled_value))
            {
                LL_WARNS() << mesh_id << "weighted_tris mismatch " << *unscaled_value << " " << ref_weighted_tris << LL_ENDL;
            }
            if (bytes && (*bytes != data.getSizeTotal()))
            {
                LL_WARNS() << mesh_id << "bytes mismatch " << *bytes << " " << data.getSizeTotal() << LL_ENDL;
            }
            if (bytes_visible && (lod >=0) && (lod < LLVolumeLODGroup::NUM_LODS) && (*bytes_visible != data.getSizeByLOD(lod)))
            {
                LL_WARNS() << mesh_id << "bytes_visible mismatch " << *bytes_visible << " " << data.getSizeByLOD(lod) << LL_ENDL;
            }
        }
        else
        {
            LL_WARNS() << "getCostData failed!!!" << LL_ENDL;
        }
    }
    return result;
}

// FIXME replace with calc based on LLMeshCostData
//static
F32 LLMeshRepository::getStreamingCostLegacy(LLMeshHeader& header, F32 radius, S32* bytes, S32* bytes_visible, S32 lod, F32 *unscaled_value)
{
    if (header.m404
        || header.mLodSize[0] <= 0
        || (header.mVersion > MAX_MESH_VERSION))
    {
        return 0.f;
    }

    F32 max_distance = 512.f;

    F32 dlowest = llmin(radius/0.03f, max_distance);
    F32 dlow = llmin(radius/0.06f, max_distance);
    F32 dmid = llmin(radius/0.24f, max_distance);

    static LLCachedControl<U32> metadata_discount_ch(gSavedSettings, "MeshMetaDataDiscount", 384);  //discount 128 bytes to cover the cost of LLSD tags and compression domain overhead
    static LLCachedControl<U32> minimum_size_ch(gSavedSettings, "MeshMinimumByteSize", 16); //make sure nothing is "free"
    static LLCachedControl<U32> bytes_per_triangle_ch(gSavedSettings, "MeshBytesPerTriangle", 16);

    F32 metadata_discount = (F32)metadata_discount_ch;
    F32 minimum_size = (F32)minimum_size_ch;
    F32 bytes_per_triangle = (F32)bytes_per_triangle_ch;

    S32 bytes_lowest = header.mLodSize[0];
    S32 bytes_low = header.mLodSize[1];
    S32 bytes_mid = header.mLodSize[2];
    S32 bytes_high = header.mLodSize[3];

    if (bytes_high == 0)
    {
        return 0.f;
    }

    if (bytes_mid == 0)
    {
        bytes_mid = bytes_high;
    }

    if (bytes_low == 0)
    {
        bytes_low = bytes_mid;
    }

    if (bytes_lowest == 0)
    {
        bytes_lowest = bytes_low;
    }

    F32 triangles_lowest = llmax((F32) bytes_lowest-metadata_discount, minimum_size)/bytes_per_triangle;
    F32 triangles_low = llmax((F32) bytes_low-metadata_discount, minimum_size)/bytes_per_triangle;
    F32 triangles_mid = llmax((F32) bytes_mid-metadata_discount, minimum_size)/bytes_per_triangle;
    F32 triangles_high = llmax((F32) bytes_high-metadata_discount, minimum_size)/bytes_per_triangle;

    if (bytes)
    {
        *bytes = 0;
        *bytes += header.mLodSize[0];
        *bytes += header.mLodSize[1];
        *bytes += header.mLodSize[2];
        *bytes += header.mLodSize[3];
    }

    if (bytes_visible)
    {
        lod = LLMeshRepository::getActualMeshLOD(header, lod);
        if (lod >= 0 && lod <= 3)
        {
            *bytes_visible = header.mLodSize[lod];
        }
    }

    F32 max_area = 102944.f; //area of circle that encompasses region (see MAINT-6559)
    F32 min_area = 1.f;

    F32 high_area = llmin(F_PI*dmid*dmid, max_area);
    F32 mid_area = llmin(F_PI*dlow*dlow, max_area);
    F32 low_area = llmin(F_PI*dlowest*dlowest, max_area);
    F32 lowest_area = max_area;

    lowest_area -= low_area;
    low_area -= mid_area;
    mid_area -= high_area;

    high_area = llclamp(high_area, min_area, max_area);
    mid_area = llclamp(mid_area, min_area, max_area);
    low_area = llclamp(low_area, min_area, max_area);
    lowest_area = llclamp(lowest_area, min_area, max_area);

    F32 total_area = high_area + mid_area + low_area + lowest_area;
    high_area /= total_area;
    mid_area /= total_area;
    low_area /= total_area;
    lowest_area /= total_area;

    F32 weighted_avg = triangles_high*high_area +
                       triangles_mid*mid_area +
                       triangles_low*low_area +
                       triangles_lowest*lowest_area;

    if (unscaled_value)
    {
        *unscaled_value = weighted_avg;
    }

    static LLCachedControl<U32> mesh_triangle_budget(gSavedSettings, "MeshTriangleBudget");
    return weighted_avg / mesh_triangle_budget * 15000.f;
}

LLMeshCostData::LLMeshCostData()
{
    std::fill(mSizeByLOD.begin(), mSizeByLOD.end(), 0);
    std::fill(mEstTrisByLOD.begin(), mEstTrisByLOD.end(), 0.f);
}

bool LLMeshCostData::init(const LLMeshHeader& header)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;

    std::fill(mSizeByLOD.begin(), mSizeByLOD.end(), 0);
    std::fill(mEstTrisByLOD.begin(), mEstTrisByLOD.end(), 0.f);

    S32 bytes_high = header.mLodSize[3];
    S32 bytes_med = header.mLodSize[2];
    if (bytes_med == 0)
    {
        bytes_med = bytes_high;
    }
    S32 bytes_low = header.mLodSize[1];
    if (bytes_low == 0)
    {
        bytes_low = bytes_med;
    }
    S32 bytes_lowest = header.mLodSize[0];
    if (bytes_lowest == 0)
    {
        bytes_lowest = bytes_low;
    }

    mSizeByLOD[0] = bytes_lowest;
    mSizeByLOD[1] = bytes_low;
    mSizeByLOD[2] = bytes_med;
    mSizeByLOD[3] = bytes_high;

    static LLCachedControl<U32> metadata_discount(gSavedSettings, "MeshMetaDataDiscount", 384);  //discount 128 bytes to cover the cost of LLSD tags and compression domain overhead
    static LLCachedControl<U32> minimum_size(gSavedSettings, "MeshMinimumByteSize", 16); //make sure nothing is "free"
    static LLCachedControl<U32> bytes_per_triangle(gSavedSettings, "MeshBytesPerTriangle", 16);

    for (S32 i=0; i<LLVolumeLODGroup::NUM_LODS; i++)
    {
        mEstTrisByLOD[i] = llmax((F32)mSizeByLOD[i] - (F32)metadata_discount, (F32)minimum_size) / (F32)bytes_per_triangle;
    }

    return true;
}


S32 LLMeshCostData::getSizeByLOD(S32 lod) const
{
    if (llclamp(lod,0,3) != lod)
    {
        return 0;
    }
    return mSizeByLOD[lod];
}

S32 LLMeshCostData::getSizeTotal() const
{
    return mSizeByLOD[0] + mSizeByLOD[1] + mSizeByLOD[2] + mSizeByLOD[3];
}

F32 LLMeshCostData::getEstTrisByLOD(S32 lod) const
{
    if (llclamp(lod,0,3) != lod)
    {
        return 0.f;
    }
    return mEstTrisByLOD[lod];
}

F32 LLMeshCostData::getEstTrisMax() const
{
    return llmax(mEstTrisByLOD[0], mEstTrisByLOD[1], mEstTrisByLOD[2], mEstTrisByLOD[3]);
}

F32 LLMeshCostData::getRadiusWeightedTris(F32 radius) const
{
    F32 max_distance = 512.f;

    F32 dlowest = llmin(radius/0.03f, max_distance);
    F32 dlow = llmin(radius/0.06f, max_distance);
    F32 dmid = llmin(radius/0.24f, max_distance);

    F32 triangles_lowest = mEstTrisByLOD[0];
    F32 triangles_low = mEstTrisByLOD[1];
    F32 triangles_mid = mEstTrisByLOD[2];
    F32 triangles_high = mEstTrisByLOD[3];

    F32 max_area = 102944.f; //area of circle that encompasses region (see MAINT-6559)
    F32 min_area = 1.f;

    F32 high_area = llmin(F_PI*dmid*dmid, max_area);
    F32 mid_area = llmin(F_PI*dlow*dlow, max_area);
    F32 low_area = llmin(F_PI*dlowest*dlowest, max_area);
    F32 lowest_area = max_area;

    lowest_area -= low_area;
    low_area -= mid_area;
    mid_area -= high_area;

    high_area = llclamp(high_area, min_area, max_area);
    mid_area = llclamp(mid_area, min_area, max_area);
    low_area = llclamp(low_area, min_area, max_area);
    lowest_area = llclamp(lowest_area, min_area, max_area);

    F32 total_area = high_area + mid_area + low_area + lowest_area;
    high_area /= total_area;
    mid_area /= total_area;
    low_area /= total_area;
    lowest_area /= total_area;

    F32 weighted_avg = triangles_high*high_area +
                       triangles_mid*mid_area +
                       triangles_low*low_area +
                       triangles_lowest*lowest_area;

    return weighted_avg;
}

F32 LLMeshCostData::getEstTrisForStreamingCost() const
{
    LL_DEBUGS("StreamingCost") << "tris_by_lod: "
                               << mEstTrisByLOD[0] << ", "
                               << mEstTrisByLOD[1] << ", "
                               << mEstTrisByLOD[2] << ", "
                               << mEstTrisByLOD[3] << LL_ENDL;

    F32 charged_tris = mEstTrisByLOD[3];
    F32 allowed_tris = mEstTrisByLOD[3];
    constexpr F32 ENFORCE_FLOOR = 64.0f;
    for (S32 i=2; i>=0; i--)
    {
        // How many tris can we have in this LOD without affecting land impact?
        // - normally an LOD should be at most half the size of the previous one.
        // - once we reach a floor of ENFORCE_FLOOR, don't require LODs to get any smaller.
        allowed_tris = llclamp(allowed_tris/2.0f,ENFORCE_FLOOR,mEstTrisByLOD[i]);
        F32 excess_tris = mEstTrisByLOD[i]-allowed_tris;
        if (excess_tris>0.f)
        {
            LL_DEBUGS("StreamingCost") << "excess tris in lod[" << i << "] " << excess_tris << " allowed " << allowed_tris <<  LL_ENDL;
            charged_tris += excess_tris;
        }
    }
    return charged_tris;
}

F32 LLMeshCostData::getRadiusBasedStreamingCost(F32 radius) const
{
    static LLCachedControl<U32> mesh_triangle_budget(gSavedSettings, "MeshTriangleBudget");
    return getRadiusWeightedTris(radius)/mesh_triangle_budget*15000.f;
}

F32 LLMeshCostData::getTriangleBasedStreamingCost() const
{
    F32 result = ANIMATED_OBJECT_COST_PER_KTRI * 0.001f * getEstTrisForStreamingCost();
    return result;
}

bool LLMeshRepository::getCostData(LLUUID mesh_id, LLMeshCostData& data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    data = LLMeshCostData();

    if (mThread && mesh_id.notNull())
    {
        LLMutexLock lock(mThread->mHeaderMutex);
        LLMeshRepoThread::mesh_header_map::iterator iter = mThread->mMeshHeader.find(mesh_id);
        if (iter != mThread->mMeshHeader.end() && iter->second.mHeaderSize > 0)
        {
            LLMeshHeader& header = iter->second;

            bool header_invalid = (header.m404
                                   || header.mLodSize[0] <= 0
                                   || header.mVersion > MAX_MESH_VERSION);
            if (!header_invalid)
            {
                return getCostData(header, data);
            }

            return true;
        }
    }
    return false;
}

// <FS:ND> Use a const ref, just to make sure no one modifies header and we can pass a copy.
// bool LLMeshRepository::getCostData(LLSD& header, LLMeshCostData& data)
bool LLMeshRepository::getCostData(const LLMeshHeader& header, LLMeshCostData& data)
// </FS:ND>
{
    data = LLMeshCostData();

    if (!data.init(header))
    {
        return false;
    }

    return true;
}

LLPhysicsDecomp::LLPhysicsDecomp()
: LLThread("Physics Decomp")
{
    mInited = false;
    mQuitting = false;
    mDone = false;

    mSignal = new LLCondition();
    mMutex = new LLMutex();
}

LLPhysicsDecomp::~LLPhysicsDecomp()
{
    shutdown();

    delete mSignal;
    mSignal = NULL;
    delete mMutex;
    mMutex = NULL;
}

void LLPhysicsDecomp::shutdown()
{
    if (mSignal)
    {
        mQuitting = true;
        // There is only one wait(), but just in case 'broadcast'
        mSignal->broadcast();

        while (!isStopped())
        {
            apr_sleep(10);
        }
    }
}

void LLPhysicsDecomp::submitRequest(LLPhysicsDecomp::Request* request)
{
    LLMutexLock lock(mMutex);
    mRequestQ.push(request);
    mSignal->signal();
}

//static
S32 LLPhysicsDecomp::llcdCallback(const char* status, S32 p1, S32 p2)
{
    if (gMeshRepo.mDecompThread && gMeshRepo.mDecompThread->mCurRequest.notNull())
    {
        return gMeshRepo.mDecompThread->mCurRequest->statusCallback(status, p1, p2);
    }

    return 1;
}

bool needTriangles( LLConvexDecomposition *aDC )
{
    if( !aDC )
        return false;

    LLCDParam const  *pParams(0);
    int nParams = aDC->getParameters( &pParams );

    if( nParams <= 0 )
        return false;

    for( int i = 0; i < nParams; ++i )
    {
        if( pParams[i].mName && strcmp( "nd_AlwaysNeedTriangles", pParams[i].mName ) == 0 )
        {
            if( LLCDParam::LLCD_BOOLEAN == pParams[i].mType && pParams[i].mDefault.mBool )
                return true;
            else
                return false;
        }
    }

    return false;
}

void LLPhysicsDecomp::setMeshData(LLCDMeshData& mesh, bool vertex_based)
{
    LLConvexDecomposition *pDeComp = LLConvexDecomposition::getInstance();

    if( !pDeComp )
        return;

    if( vertex_based )
        vertex_based = !needTriangles( pDeComp );

    mesh.mVertexBase = mCurRequest->mPositions[0].mV;
    mesh.mVertexStrideBytes = 12;
    mesh.mNumVertices = static_cast<int>(mCurRequest->mPositions.size());

    if(!vertex_based)
    {
        mesh.mIndexType = LLCDMeshData::INT_16;
        mesh.mIndexBase = &(mCurRequest->mIndices[0]);
        mesh.mIndexStrideBytes = 6;

        mesh.mNumTriangles = static_cast<int>(mCurRequest->mIndices.size())/3;
    }

    if ((vertex_based || mesh.mNumTriangles > 0) && mesh.mNumVertices > 2)
    {
        LLCDResult ret = LLCD_OK;
        ret  = LLConvexDecomposition::getInstance()->setMeshData(&mesh, vertex_based);

        if (ret)
            LL_ERRS(LOG_MESH) << "Convex Decomposition thread valid but could not set mesh data." << LL_ENDL;
    }
}

void LLPhysicsDecomp::doDecomposition()
{
    LLCDMeshData mesh;
    S32 stage = mStageID[mCurRequest->mStage];

    if (LLConvexDecomposition::getInstance() == NULL)
    {
        // stub library. do nothing.
        return;
    }

    //load data intoLLCD
    if (stage == 0)
    {
        setMeshData(mesh, false);
    }

    //build parameter map
    std::map<std::string, const LLCDParam*> param_map;

    static const LLCDParam* params = NULL;
    static S32 param_count = 0;
    if (!params)
    {
        param_count = LLConvexDecomposition::getInstance()->getParameters(&params);
    }

    for (S32 i = 0; i < param_count; ++i)
    {
        param_map[params[i].mName] = params+i;
    }

    U32 ret = LLCD_OK;
    //set parameter values
    for (decomp_params::iterator iter = mCurRequest->mParams.begin(); iter != mCurRequest->mParams.end(); ++iter)
    {
        const std::string& name = iter->first;
        const LLSD& value = iter->second;

        const LLCDParam* param = param_map[name];

        if (param == NULL)
        { //couldn't find valid parameter
            continue;
        }


        if (param->mType == LLCDParam::LLCD_FLOAT)
        {
            ret = LLConvexDecomposition::getInstance()->setParam(param->mName, (F32) value.asReal());
        }
        else if (param->mType == LLCDParam::LLCD_INTEGER ||
            param->mType == LLCDParam::LLCD_ENUM)
        {
            ret = LLConvexDecomposition::getInstance()->setParam(param->mName, value.asInteger());
        }
        else if (param->mType == LLCDParam::LLCD_BOOLEAN)
        {
            ret = LLConvexDecomposition::getInstance()->setParam(param->mName, value.asBoolean());
        }
    }

    mCurRequest->setStatusMessage("Executing.");

    if (LLConvexDecomposition::getInstance() != NULL)
    {
        ret = LLConvexDecomposition::getInstance()->executeStage(stage);
    }

    if (ret)
    {
        LL_WARNS(LOG_MESH) << "Convex Decomposition thread valid but could not execute stage " << stage << "."
                           << LL_ENDL;
        LLMutexLock lock(mMutex);

        mCurRequest->mHull.clear();
        mCurRequest->mHullMesh.clear();

        mCurRequest->setStatusMessage("FAIL");

        completeCurrent();
    }
    else
    {
        mCurRequest->setStatusMessage("Reading results");

        S32 num_hulls =0;
        if (LLConvexDecomposition::getInstance() != NULL)
        {
            num_hulls = LLConvexDecomposition::getInstance()->getNumHullsFromStage(stage);
        }

        {
            LLMutexLock lock(mMutex);
            mCurRequest->mHull.clear();
            mCurRequest->mHull.resize(num_hulls);

            mCurRequest->mHullMesh.clear();
            mCurRequest->mHullMesh.resize(num_hulls);
        }

        for (S32 i = 0; i < num_hulls; ++i)
        {
            std::vector<LLVector3> p;
            LLCDHull hull;
            // if LLConvexDecomposition is a stub, num_hulls should have been set to 0 above, and we should not reach this code
            LLConvexDecomposition::getInstance()->getHullFromStage(stage, i, &hull);

            const F32* v = hull.mVertexBase;

            for (S32 j = 0; j < hull.mNumVertices; ++j)
            {
                LLVector3 vert(v[0], v[1], v[2]);
                p.push_back(vert);
                v = (F32*) (((U8*) v) + hull.mVertexStrideBytes);
            }

            LLCDMeshData mesh;
            // if LLConvexDecomposition is a stub, num_hulls should have been set to 0 above, and we should not reach this code
            LLConvexDecomposition::getInstance()->getMeshFromStage(stage, i, &mesh);

            get_vertex_buffer_from_mesh(mesh, mCurRequest->mHullMesh[i]);

            {
                LLMutexLock lock(mMutex);
                mCurRequest->mHull[i] = p;
            }
        }

        {
            LLMutexLock lock(mMutex);
            mCurRequest->setStatusMessage("FAIL");
            completeCurrent();
        }
    }
}

void LLPhysicsDecomp::completeCurrent()
{
    LLMutexLock lock(mMutex);
    mCompletedQ.push(mCurRequest);
    mCurRequest = NULL;
}

void LLPhysicsDecomp::notifyCompleted()
{
    if (!mCompletedQ.empty())
    {
        LLMutexLock lock(mMutex);
        while (!mCompletedQ.empty())
        {
            Request* req = mCompletedQ.front();
            req->completed();
            mCompletedQ.pop();
        }
    }
}


void make_box(LLPhysicsDecomp::Request * request)
{
    LLVector3 min,max;
    min = request->mPositions[0];
    max = min;

    for (U32 i = 0; i < request->mPositions.size(); ++i)
    {
        update_min_max(min, max, request->mPositions[i]);
    }

    request->mHull.clear();

    LLModel::hull box;
    box.push_back(LLVector3(min[0],min[1],min[2]));
    box.push_back(LLVector3(max[0],min[1],min[2]));
    box.push_back(LLVector3(min[0],max[1],min[2]));
    box.push_back(LLVector3(max[0],max[1],min[2]));
    box.push_back(LLVector3(min[0],min[1],max[2]));
    box.push_back(LLVector3(max[0],min[1],max[2]));
    box.push_back(LLVector3(min[0],max[1],max[2]));
    box.push_back(LLVector3(max[0],max[1],max[2]));

    request->mHull.push_back(box);
}


void LLPhysicsDecomp::doDecompositionSingleHull()
{
    LLConvexDecomposition* decomp = LLConvexDecomposition::getInstance();

    if (decomp == NULL)
    {
        //stub. do nothing.
        return;
    }

    LLCDMeshData mesh;

    setMeshData(mesh, true);

    LLCDResult ret = decomp->buildSingleHull() ;
    if (ret)
    {
        LL_WARNS(LOG_MESH) << "Could not execute decomposition stage when attempting to create single hull." << LL_ENDL;
        make_box(mCurRequest);
    }
    else
    {
        {
            LLMutexLock lock(mMutex);
            mCurRequest->mHull.clear();
            mCurRequest->mHull.resize(1);
            mCurRequest->mHullMesh.clear();
        }

        std::vector<LLVector3> p;
        LLCDHull hull;

        // if LLConvexDecomposition is a stub, num_hulls should have been set to 0 above, and we should not reach this code
        decomp->getSingleHull(&hull);

        const F32* v = hull.mVertexBase;

        for (S32 j = 0; j < hull.mNumVertices; ++j)
        {
            LLVector3 vert(v[0], v[1], v[2]);
            p.push_back(vert);
            v = (F32*) (((U8*) v) + hull.mVertexStrideBytes);
        }

        {
            LLMutexLock lock(mMutex);
            mCurRequest->mHull[0] = p;
        }
    }

    {
        completeCurrent();

    }
}


void LLPhysicsDecomp::run()
{
    LLConvexDecomposition* decomp = LLConvexDecomposition::getInstance();
    if (decomp == NULL)
    {
        // stub library. Set init to true so the main thread
        // doesn't wait for this to finish.
        mInited = true;
        return;
    }

    decomp->initThread();
    mInited = true;

    static const LLCDStageData* stages = NULL;
    static S32 num_stages = 0;

    if (!stages)
    {
        num_stages = decomp->getStages(&stages);
    }

    for (S32 i = 0; i < num_stages; i++)
    {
        mStageID[stages[i].mName] = i;
    }

    while (!mQuitting)
    {
        mSignal->wait();
        while (!mQuitting && !mRequestQ.empty())
        {
            {
                LLMutexLock lock(mMutex);
                mCurRequest = mRequestQ.front();
                mRequestQ.pop();
            }

            S32& id = *(mCurRequest->mDecompID);
            if (id == -1)
            {
                decomp->genDecomposition(id);
            }
            decomp->bindDecomposition(id);

            if (mCurRequest->mStage == "single_hull")
            {
                doDecompositionSingleHull();
            }
            else
            {
                doDecomposition();
            }
        }
    }

    decomp->quitThread();

    if (mSignal->isLocked())
    { //let go of mSignal's associated mutex
        mSignal->unlock();
    }

    mDone = true;
}

void LLPhysicsDecomp::Request::assignData(LLModel* mdl)
{
    if (!mdl)
    {
        return;
    }

    U16 index_offset = 0;
    U16 tri[3]{};

    mPositions.clear();
    mIndices.clear();
    mBBox[1] = LLVector3(F32_MIN, F32_MIN, F32_MIN);
    mBBox[0] = LLVector3(F32_MAX, F32_MAX, F32_MAX);

    //queue up vertex positions and indices
    for (S32 i = 0; i < mdl->getNumVolumeFaces(); ++i)
    {
        const LLVolumeFace& face = mdl->getVolumeFace(i);
        if (mPositions.size() + face.mNumVertices > 65535)
        {
            continue;
        }

        for (S32 j = 0; j < face.mNumVertices; ++j)
        {
            mPositions.push_back(LLVector3(face.mPositions[j].getF32ptr()));
            for (U32 k = 0 ; k < 3 ; k++)
            {
                mBBox[0].mV[k] = llmin(mBBox[0].mV[k], mPositions[j].mV[k]);
                mBBox[1].mV[k] = llmax(mBBox[1].mV[k], mPositions[j].mV[k]);
            }
        }

        updateTriangleAreaThreshold();

        for (S32 j = 0; j+2 < face.mNumIndices; j += 3)
        {
            tri[0] = face.mIndices[j] + index_offset ;
            tri[1] = face.mIndices[j + 1] + index_offset;
            tri[2] = face.mIndices[j + 2] + index_offset;

            if (isValidTriangle(tri[0], tri[1], tri[2]))
            {
                mIndices.emplace_back(tri[0]);
                mIndices.emplace_back(tri[1]);
                mIndices.emplace_back(tri[2]);
            }
        }

        index_offset += face.mNumVertices;
    }
}

void LLPhysicsDecomp::Request::updateTriangleAreaThreshold()
{
    F32 range = mBBox[1].mV[0] - mBBox[0].mV[0] ;
    range = llmin(range, mBBox[1].mV[1] - mBBox[0].mV[1]) ;
    range = llmin(range, mBBox[1].mV[2] - mBBox[0].mV[2]) ;

    mTriangleAreaThreshold = llmin(0.0002f, range * 0.000002f) ;
}

//check if the triangle area is large enough to qualify for a valid triangle
bool LLPhysicsDecomp::Request::isValidTriangle(U16 idx1, U16 idx2, U16 idx3)
{
    LLVector3 a = mPositions[idx2] - mPositions[idx1] ;
    LLVector3 b = mPositions[idx3] - mPositions[idx1] ;
    F32 c = a * b ;

    return ((a*a) * (b*b) - c * c) > mTriangleAreaThreshold ;
}

void LLPhysicsDecomp::Request::setStatusMessage(const std::string& msg)
{
    mStatusMessage = msg;
}

void LLMeshRepository::buildPhysicsMesh(LLModel::Decomposition& decomp)
{
    decomp.mMesh.resize(decomp.mHull.size());

    for (size_t i = 0; i < decomp.mHull.size(); ++i)
    {
        LLCDHull hull;
        hull.mNumVertices = static_cast<int>(decomp.mHull[i].size());
        hull.mVertexBase = decomp.mHull[i][0].mV;
        hull.mVertexStrideBytes = 12;

        LLCDMeshData mesh;
        LLCDResult res = LLCD_OK;
        if (LLConvexDecomposition::getInstance() != NULL)
        {
            res = LLConvexDecomposition::getInstance()->getMeshFromHull(&hull, &mesh);
        }
        if (res == LLCD_OK)
        {
            get_vertex_buffer_from_mesh(mesh, decomp.mMesh[i]);
        }
    }

    if (!decomp.mBaseHull.empty() && decomp.mBaseHullMesh.empty())
    { //get mesh for base hull
        LLCDHull hull;
        hull.mNumVertices = static_cast<int>(decomp.mBaseHull.size());
        hull.mVertexBase = decomp.mBaseHull[0].mV;
        hull.mVertexStrideBytes = 12;

        LLCDMeshData mesh;
        LLCDResult res = LLCD_OK;
        if (LLConvexDecomposition::getInstance() != NULL)
        {
            res = LLConvexDecomposition::getInstance()->getMeshFromHull(&hull, &mesh);
        }
        if (res == LLCD_OK)
        {
            get_vertex_buffer_from_mesh(mesh, decomp.mBaseHullMesh);
        }
    }
}


bool LLMeshRepository::meshUploadEnabled()
{
    LLViewerRegion *region = gAgent.getRegion();
    // <FS:Ansariel> Use faster LLCachedControls for frequently visited locations
    //if(gSavedSettings.getBOOL("MeshEnabled") &&
    static LLCachedControl<bool> meshEnabled(gSavedSettings, "MeshEnabled");
    if(meshEnabled &&
    // </FS:Ansariel>
       region)
    {
        return region->meshUploadEnabled();
    }
    return false;
}

bool LLMeshRepository::meshRezEnabled()
{
    static LLCachedControl<bool> mesh_enabled(gSavedSettings, "MeshEnabled");
// <FS:Beq> FIRE-35602 etc - Mesh not appearing after TP/login (opensim only)
// For OpenSim there is still an outside chance that mesh rezzing is disabled on the sim/region
// restore the old behaviour but keep the bias to mesh_enabled == true in the underlying checks.
#ifdef OPENSIM
    if (LLGridManager::instance().isInOpenSim())
    {
        if (LLViewerRegion* region = gAgent.getRegion(); mesh_enabled && region)
        {
            return region->meshRezEnabled();
        }
    }
#endif // OPENSIM
// </FS:Beq>
    return mesh_enabled;
}

// Threading:  main thread only
// static
void LLMeshRepository::metricsStart()
{
    ++metrics_teleport_start_count;
    sQuiescentTimer.start(0);
}

// Threading:  main thread only
// static
void LLMeshRepository::metricsStop()
{
    sQuiescentTimer.stop(0);
}

// Threading:  main thread only
// static
void LLMeshRepository::metricsProgress(unsigned int this_count)
{
    static bool first_start(true);

    if (first_start)
    {
        metricsStart();
        first_start = false;
    }
    sQuiescentTimer.ringBell(0, this_count);
}

// Threading:  main thread only
// static
void LLMeshRepository::metricsUpdate()
{
    F64 started, stopped;
    U64 total_count(U64L(0)), user_cpu(U64L(0)), sys_cpu(U64L(0));

    if (sQuiescentTimer.isExpired(0, started, stopped, total_count, user_cpu, sys_cpu))
    {
        LLSD metrics;

        metrics["reason"] = "Mesh Download Quiescent";
        metrics["scope"] = metrics_teleport_start_count > 1 ? "Teleport" : "Login";
        metrics["start"] = started;
        metrics["stop"] = stopped;
        metrics["fetches"] = LLSD::Integer(total_count);
        metrics["teleports"] = LLSD::Integer(metrics_teleport_start_count);
        metrics["user_cpu"] = double(user_cpu) / 1.0e6;
        metrics["sys_cpu"] = double(sys_cpu) / 1.0e6;
        LL_INFOS(LOG_MESH) << "EventMarker " << metrics << LL_ENDL;
    }
}

// Threading:  main thread only
// static
void teleport_started()
{
    LLMeshRepository::metricsStart();
}


void on_new_single_inventory_upload_complete(
    LLAssetType::EType asset_type,
    LLInventoryType::EType inventory_type,
    const std::string inventory_type_string,
    const LLUUID& item_folder_id,
    const std::string& item_name,
    const std::string& item_description,
    const LLSD& server_response,
    S32 upload_price)
{
    bool success = false;

    if (upload_price > 0)
    {
        // this upload costed us L$, update our balance
        // and display something saying that it cost L$
        LLStatusBar::sendMoneyBalanceRequest();

        // <FS:Ansariel> FIRE-10628 - Option to supress upload cost notification
        if (gSavedSettings.getBOOL("FSShowUploadPaymentToast"))
        {
        LLSD args;
        args["AMOUNT"] = llformat("%d", upload_price);
        LLNotificationsUtil::add("UploadPayment", args);
        }
        // </FS:Ansariel>
    }

    if (item_folder_id.notNull())
    {
        U32 everyone_perms = PERM_NONE;
        U32 group_perms = PERM_NONE;
        U32 next_owner_perms = PERM_ALL;
        if (server_response.has("new_next_owner_mask"))
        {
            // The server provided creation perms so use them.
            // Do not assume we got the perms we asked for in
            // since the server may not have granted them all.
            everyone_perms = server_response["new_everyone_mask"].asInteger();
            group_perms = server_response["new_group_mask"].asInteger();
            next_owner_perms = server_response["new_next_owner_mask"].asInteger();
        }
        else
        {
            // The server doesn't provide creation perms
            // so use old assumption-based perms.
            if (inventory_type_string != "snapshot")
            {
                next_owner_perms = PERM_MOVE | PERM_TRANSFER;
            }
        }

        LLPermissions new_perms;
        new_perms.init(
            gAgent.getID(),
            gAgent.getID(),
            LLUUID::null,
            LLUUID::null);

        new_perms.initMasks(
            PERM_ALL,
            PERM_ALL,
            everyone_perms,
            group_perms,
            next_owner_perms);

        U32 inventory_item_flags = 0;
        if (server_response.has("inventory_flags"))
        {
            inventory_item_flags = (U32)server_response["inventory_flags"].asInteger();
            if (inventory_item_flags != 0)
            {
                LL_INFOS() << "inventory_item_flags " << inventory_item_flags << LL_ENDL;
            }
        }
        S32 creation_date_now = (S32)time_corrected();
        LLPointer<LLViewerInventoryItem> item = new LLViewerInventoryItem(
            server_response["new_inventory_item"].asUUID(),
            item_folder_id,
            new_perms,
            server_response["new_asset"].asUUID(),
            asset_type,
            inventory_type,
            item_name,
            item_description,
            LLSaleInfo::DEFAULT,
            inventory_item_flags,
            creation_date_now);

        gInventory.updateItem(item);
        gInventory.notifyObservers();
        success = true;

        LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();

        // Show the preview panel for textures and sounds to let
        // user know that the image (or snapshot) arrived intact.
        LLInventoryPanel* panel = LLInventoryPanel::getActiveInventoryPanel(false);
        if (panel)
        {

            panel->setSelection(
                server_response["new_inventory_item"].asUUID(),
                TAKE_FOCUS_NO);
        }
        else
        {
            LLInventoryPanel::openInventoryPanelAndSetSelection(true, server_response["new_inventory_item"].asUUID(), true, false, true);
        }

        // restore keyboard focus
        gFocusMgr.setKeyboardFocus(focus);
    }
    else
    {
        LL_WARNS() << "Can't find a folder to put it in" << LL_ENDL;
    }

    // Todo: This is mesh repository code, is following code really needed?
    // remove the "Uploading..." message
    LLUploadDialog::modalUploadFinished();

    // Let the Snapshot floater know we have finished uploading a snapshot to inventory.
    LLFloater* floater_snapshot = LLFloaterReg::findInstance("snapshot");
    if (asset_type == LLAssetType::AT_TEXTURE && floater_snapshot)
    {
        floater_snapshot->notify(LLSD().with("set-finished", LLSD().with("ok", success).with("msg", "inventory")));
    }
}
