/**
 * @file llstyle.cpp
 * @brief Text style class
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#include "linden_common.h"

#include "llstyle.h"

#include "llfontgl.h"
#include "llstring.h"
#include "llui.h"

LLStyle::Params::Params()
    : visible("visible", true),
    drop_shadow("drop_shadow", LLFontGL::NO_SHADOW),
    color("color", LLColor4::black),
    readonly_color("readonly_color", LLColor4::black),
    selected_color("selected_color", LLColor4::black),
    highlight_bg_color("highlight_bg_color", LLColor4::green),
    alpha("alpha", 1.f),
    font("font", LLStyle::getDefaultFont()),
    image("image"),
    link_href("href"),
    is_link("is_link"),
    draw_highlight_bg("draw_highlight_bg", false),
    // <FS:Ansariel> Don't highlight URLs on hover if font style contains underline
    use_default_link_style("use_default_link_style", true),
    can_underline_on_hover("can_underline_on_hover", true)
    // </FS:Ansariel>
{}


LLStyle::LLStyle(const LLStyle::Params& p)
:   mVisible(p.visible),
    mColor(p.color),
    mReadOnlyColor(p.readonly_color),
    mSelectedColor(p.selected_color),
    mHighlightBgColor(p.highlight_bg_color),
    mFont(p.font()),
    mLink(p.link_href),
    mIsLink(p.is_link.isProvided() ? p.is_link : !p.link_href().empty()),
    mDropShadow(p.drop_shadow),
    mImagep(p.image()),
    mAlpha(p.alpha),
    mDrawHighlightBg(p.draw_highlight_bg)
{}

void LLStyle::setFont(const LLFontGL* font)
{
    mFont = font;
}


const LLFontGL* LLStyle::getFont() const
{
    return mFont;
}

const LLFontGL* LLStyle::getDefaultFont()
{
    return LLFontGL::getFontMonospace();
}

void LLStyle::setLinkHREF(const std::string& href)
{
    mLink = href;
}

bool LLStyle::isLink() const
{
    return mIsLink;
}

bool LLStyle::isVisible() const
{
    return mVisible;
}

void LLStyle::setVisible(bool is_visible)
{
    mVisible = is_visible;
}

LLPointer<LLUIImage> LLStyle::getImage() const
{
    return mImagep;
}

void LLStyle::setImage(const LLUUID& src)
{
    mImagep = LLUI::getUIImageByID(src);
}

void LLStyle::setImage(const std::string& name)
{
    mImagep = LLUI::getUIImage(name);
}
