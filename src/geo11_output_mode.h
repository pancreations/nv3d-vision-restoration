// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>

namespace vision::geo11 {
enum class OutputMode { Unsupported, SideBySide, TopBottom, SideBySideReversed, TopBottomReversed, Katanga };
inline OutputMode outputMode(std::string_view value) {
    const auto comment=value.find_first_of(";#");
    if(comment!=std::string_view::npos)value=value.substr(0,comment);
    const auto first=value.find_first_not_of(" \t\r\n\"");
    if(first==std::string_view::npos)return OutputMode::SideBySide; // Geo11 default.
    value=value.substr(first,value.find_last_not_of(" \t\r\n\"")-first+1);
    auto equals=[&](std::string_view expected){
        if(value.size()!=expected.size())return false;
        for(size_t i=0;i<value.size();++i){char c=value[i];if(c>='A'&&c<='Z')c+=char('a'-'A');if(c!=expected[i])return false;}
        return true;
    };
    if(equals("sbs"))return OutputMode::SideBySide;
    if(equals("tab"))return OutputMode::TopBottom;
    if(equals("sbs_reversed"))return OutputMode::SideBySideReversed;
    if(equals("tab_reversed"))return OutputMode::TopBottomReversed;
    if(equals("katanga_vr"))return OutputMode::Katanga;
    return OutputMode::Unsupported;
}
inline bool vertical(OutputMode mode){return mode==OutputMode::TopBottom||mode==OutputMode::TopBottomReversed;}
inline bool reversed(OutputMode mode){return mode==OutputMode::SideBySideReversed||mode==OutputMode::TopBottomReversed;}
}
