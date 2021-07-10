/*
*      Copyright (C) 2016-2016 peak3d
*      http://www.peak3d.de
*
*  This Program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2, or (at your option)
*  any later version.
*
*  This Program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*  GNU General Public License for more details.
*
*  <http://www.gnu.org/licenses/>.
*
*/

#pragma once

#include "../common/AdaptiveTree.h"

#include <kodi/AddonBase.h>

namespace adaptive
{

class ATTRIBUTE_HIDDEN SmoothTree : public AdaptiveTree
{
public:
  SmoothTree();
  virtual bool open(const std::string& url, const std::string& manifestUpdateParam) override;
  virtual bool open(const std::string& url, const std::string& manifestUpdateParam, std::map<std::string, std::string> additionalHeaders) override;
  virtual bool write_data(void* buffer, size_t buffer_size, void* opaque) override;

  enum
  {
    SSMNODE_SSM = 1 << 0,
    SSMNODE_PROTECTION = 1 << 1,
    SSMNODE_STREAMINDEX = 1 << 2,
    SSMNODE_PROTECTIONHEADER = 1 << 3,
    SSMNODE_PROTECTIONTEXT = 1 << 4
  };

  uint64_t pts_helper_;
  };

}
