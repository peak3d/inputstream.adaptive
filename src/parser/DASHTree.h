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

class ATTRIBUTE_HIDDEN DASHTree : public AdaptiveTree
{
public:
  DASHTree();
  virtual bool open(const std::string& url, const std::string& manifestUpdateParam) override;
  virtual bool write_data(void* buffer, size_t buffer_size, void* opaque) override;
  virtual void RefreshSegments(Period* period,
                               AdaptationSet* adp,
                               Representation* rep,
                               StreamType type) override;

  virtual uint64_t GetNowTime() { return time(0); };
  virtual std::chrono::system_clock::time_point GetTimePointNowTime()
  {
    return std::chrono::system_clock::now();
  };
  virtual void SetLastUpdated(std::chrono::system_clock::time_point tm){};
  void SetUpdateInterval(uint32_t interval) { updateInterval_ = interval; };
  uint64_t pts_helper_, timeline_time_;
  uint32_t firstStartNumber_;
  std::string current_playready_wrmheader_;
  std::string mpd_url_;

protected:
  virtual void RefreshLiveSegments() override;
  };
}
