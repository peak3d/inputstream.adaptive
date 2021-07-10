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

#include "AdaptiveTree.h"
#include <string.h>
#include <algorithm>
#include <stdlib.h>
#include <chrono>
#include "../log.h"

namespace adaptive
{
  void AdaptiveTree::Segment::SetRange(const char *range)
  {
    const char *delim(strchr(range, '-'));
    if (delim)
    {
      range_begin_ = strtoull(range, 0, 10);
      range_end_ = strtoull(delim + 1, 0, 10);
    }
    else
      range_begin_ = range_end_ = 0;
  }

  void AdaptiveTree::Segment::Copy(const Segment* src)
  {
    delete[] url, url = nullptr;
    *this = *src;
    if (src->url)
    {
      size_t len = strlen(src->url) + 1;
      url = new char[len];
      memcpy((void*)url, src->url, len);
    }
  }

  AdaptiveTree::AdaptiveTree()
    : current_period_(nullptr)
    , next_period_(nullptr)
    , parser_(0)
    , currentNode_(0)
    , segcount_(0)
    , overallSeconds_(0)
    , stream_start_(0)
    , available_time_(0)
    , base_time_(0)
    , minPresentationOffset(0)
    , has_timeshift_buffer_(false)
    , has_overall_seconds_(false)
    , updateInterval_(~0)
    , updateThread_(nullptr)
    , lastUpdated_(std::chrono::system_clock::now())
  {
  }

  AdaptiveTree::~AdaptiveTree()
  {
    has_timeshift_buffer_ = false;
    if (updateThread_)
    {
      {
        std::lock_guard<std::mutex> lck(updateMutex_);
        updateVar_.notify_one();
      }
      updateThread_->join();
      delete updateThread_;
    }

    std::lock_guard<std::mutex> lck(treeMutex_);
    for (std::vector<Period*>::const_iterator bp(periods_.begin()), ep(periods_.end()); bp != ep; ++bp)
      delete *bp;
  }

  void AdaptiveTree::FreeSegments(Period* period, Representation* rep)
  {
    for (std::vector<Segment>::iterator bs(rep->segments_.data.begin()), es(rep->segments_.data.end()); bs != es; ++bs)
    {
      --period->psshSets_[bs->pssh_set_].use_count_;
      if (rep->flags_ & Representation::URLSEGMENTS)
        delete[] bs->url;
    }
    if ((rep->flags_ & (Representation::INITIALIZATION | Representation::URLSEGMENTS))
      == (Representation::INITIALIZATION | Representation::URLSEGMENTS))
      delete[]rep->initialization_.url;
    rep->segments_.clear();
    rep->current_segment_ = nullptr;
  }


  bool AdaptiveTree::has_type(StreamType t)
  {
    if (periods_.empty())
      return false;

    for (std::vector<AdaptationSet*>::const_iterator b(periods_[0]->adaptationSets_.begin()), e(periods_[0]->adaptationSets_.end()); b != e; ++b)
      if ((*b)->type_ == t)
        return true;
    return false;
  }

  uint32_t AdaptiveTree::estimate_segcount(uint64_t duration, uint32_t timescale)
  {
    duration /= timescale;
    return static_cast<uint32_t>((overallSeconds_ / duration)*1.01);
  }

  void AdaptiveTree::SetFragmentDuration(const AdaptationSet* adp, const Representation* rep, size_t pos, uint64_t timestamp, uint32_t fragmentDuration, uint32_t movie_timescale)
  {
    if (!has_timeshift_buffer_ || HasUpdateThread() ||
      (rep->flags_ & AdaptiveTree::Representation::URLSEGMENTS) != 0)
      return;

    //Get a modifiable adaptationset
    AdaptationSet *adpm(const_cast<AdaptationSet *>(adp));

    // Check if its the last frame we watch
    if (adp->segment_durations_.data.size())
    {
      if (pos == adp->segment_durations_.data.size() - 1)
      {
        adpm->segment_durations_.insert(static_cast<std::uint64_t>(fragmentDuration)*adp->timescale_ / movie_timescale);
      }
      else
      {
        ++const_cast<Representation*>(rep)->expired_segments_;
        return;
      }
    }
    else if (pos != rep->segments_.data.size() - 1)
      return;

    Segment seg(*(rep->segments_[pos]));

    if (!timestamp)
    {
      Log(LOGLEVEL_DEBUG, "AdaptiveTree: scale fragment duration: fdur:%u, rep-scale:%u, mov-scale:%u", fragmentDuration, rep->timescale_, movie_timescale);
      fragmentDuration = static_cast<std::uint32_t>((static_cast<std::uint64_t>(fragmentDuration)*rep->timescale_) / movie_timescale);
    }
    else
    {
      Log(LOGLEVEL_DEBUG, "AdaptiveTree: fragment duration from timestamp: ts:%llu, base:%llu, s-pts:%llu", timestamp, base_time_, seg.startPTS_);
      fragmentDuration = static_cast<uint32_t>(timestamp - base_time_ - seg.startPTS_);
    }

    seg.startPTS_ += fragmentDuration;
    seg.range_begin_ += fragmentDuration;
    seg.range_end_ ++;

    Log(LOGLEVEL_DEBUG, "AdaptiveTree: insert live segment: pts: %llu range_end: %llu", seg.startPTS_, seg.range_end_);

    for (std::vector<Representation*>::iterator b(adpm->representations_.begin()), e(adpm->representations_.end()); b != e; ++b)
      (*b)->segments_.insert(seg);
  }

  void AdaptiveTree::OnDataArrived(unsigned int segNum, uint16_t psshSet, uint8_t iv[16], const uint8_t *src, uint8_t *dst, size_t dstOffset, size_t dataSize)
  {
    memcpy(dst + dstOffset, src, dataSize);
  }

  uint16_t AdaptiveTree::insert_psshset(StreamType type,
                                        AdaptiveTree::Period* period,
                                        AdaptiveTree::AdaptationSet* adp)
  {
    if (!period)
      period = current_period_;
    if (!adp)
      adp = current_adaptationset_;

    if (!current_pssh_.empty())
    {
      Period::PSSH pssh;
      pssh.pssh_ = current_pssh_;
      pssh.defaultKID_ = current_defaultKID_;
      pssh.iv = current_iv_;
      pssh.adaptation_set_ = adp;
      switch (type)
      {
      case VIDEO: pssh.media_ = Period::PSSH::MEDIA_VIDEO; break;
      case AUDIO: pssh.media_ = Period::PSSH::MEDIA_AUDIO; break;
      case STREAM_TYPE_COUNT: pssh.media_ = Period::PSSH::MEDIA_VIDEO | Period::PSSH::MEDIA_AUDIO; break;
      default: pssh.media_ = 0; break;
      }
      return period->InsertPSSHSet(&pssh);
    }
    else
      return period->InsertPSSHSet(nullptr);
  }

  void AdaptiveTree::Representation::CopyBasicData(Representation* src)
  {
    url_ = src->url_;
    id = src->id;
    codecs_ = src->codecs_;
    codec_private_data_ = src->codec_private_data_;
    source_url_ = src->source_url_;
    bandwidth_ = src->bandwidth_;
    samplingRate_ = src->samplingRate_;
    width_ = src->width_;
    height_ = src->height_;
    fpsRate_ = src->fpsRate_;
    fpsScale_ = src->fpsScale_;
    aspect_ = src->aspect_;
    flags_ = src->flags_;
    hdcpVersion_ = src->hdcpVersion_;
    channelCount_ = src->channelCount_;
    nalLengthSize_ = src->nalLengthSize_;
    containerType_ = src->containerType_;
    timescale_ = src->timescale_;
    timescale_ext_ = src->timescale_ext_;
    timescale_int_ = src->timescale_int_;
  }

  void AdaptiveTree::AdaptationSet::CopyBasicData(AdaptiveTree::AdaptationSet* src)
  {
    representations_.resize(src->representations_.size());
    auto itRep = src->representations_.begin();
    for (Representation*& rep : representations_)
    {
      rep = new Representation();
      rep->CopyBasicData(*itRep++);
    }

    type_ = src->type_;
    timescale_ = src->timescale_;
    duration_ = src->duration_;
    startPTS_ = src->startPTS_;
    startNumber_ = src->startNumber_;
    impaired_ = src->impaired_;
    original_ = src->original_;
    default_ = src->default_;
    forced_ = src->forced_;
    language_ = src->language_;
    mimeType_ = src->mimeType_;
    base_url_ = src->base_url_;
    id_ = src->id_;
    group_ = src->group_;
    codecs_ = src->codecs_;
    audio_track_id_ = src->audio_track_id_;
    name_ = src->name_;
  }

  // Create a HLS master playlist copy (no representations)
  void AdaptiveTree::Period::CopyBasicData(AdaptiveTree::Period* period)
  {
    adaptationSets_.resize(period->adaptationSets_.size());
    auto itAdp = period->adaptationSets_.begin();
    for (AdaptationSet*& adp : adaptationSets_)
    {
      adp = new AdaptationSet();
      adp->CopyBasicData(*itAdp++);
    }

    base_url_ = period->base_url_;
    id_ = period->id_;
    timescale_ = period->timescale_;
    startNumber_ = period->startNumber_;

    start_ = period->start_;
    startPTS_ = period->startPTS_;
    duration_ = period->duration_;
    encryptionState_ = period->encryptionState_;
    included_types_ = period->included_types_;
    need_secure_decoder_ = period->need_secure_decoder_;
  }

  uint16_t AdaptiveTree::Period::InsertPSSHSet(PSSH* pssh)
  {
    if (pssh)
    {
      std::vector<Period::PSSH>::iterator pos(std::find(psshSets_.begin() + 1, psshSets_.end(), *pssh));
      if (pos == psshSets_.end())
        pos = psshSets_.insert(psshSets_.end(), *pssh);
      else if (!pos->use_count_)
        *pos = *pssh;

      ++psshSets_[pos - psshSets_.begin()].use_count_;
      return static_cast<uint16_t>(pos - psshSets_.begin());
    }
    else
    {
      ++psshSets_[0].use_count_;
      return 0;
    }
  }

  void AdaptiveTree::Period::RemovePSSHSet(uint16_t pssh_set)
  {
    for (std::vector<AdaptationSet*>::const_iterator ba(adaptationSets_.begin()), ea(adaptationSets_.end()); ba != ea; ++ba)
      for (std::vector<Representation*>::iterator br((*ba)->representations_.begin()), er((*ba)->representations_.end()); br != er;)
        if ((*br)->pssh_set_ == pssh_set)
        {
          delete *br;
          br = (*ba)->representations_.erase(br);
          er = (*ba)->representations_.end();
        }
        else
          ++br;
  }

  bool AdaptiveTree::PreparePaths(const std::string &url)
  {
    manifest_url_ = url;

    size_t paramPos = url.find('?');
    base_url_ = (paramPos == std::string::npos) ? url : url.substr(0, paramPos);

    paramPos = base_url_.find_last_of('/', base_url_.length());
    if (paramPos == std::string::npos)
    {
      Log(LOGLEVEL_ERROR, "Invalid url: / expected (%s)", url.c_str());
      return false;
    }
    base_url_.resize(paramPos + 1);

    paramPos = base_url_.find("://");
    if (paramPos != std::string::npos)
    {
      base_domain_ = base_url_;
      paramPos = base_domain_.find_first_of('/', paramPos + 3);
      if (paramPos != std::string::npos)
        base_domain_.resize(paramPos);
    }
    else
      base_domain_.clear();

    return true;
  }

  void AdaptiveTree::PrepareManifestUrl(const std::string &url, const std::string &manifestUpdateParam)
  {
    manifest_url_ = url;

    if (manifestUpdateParam.empty())
    {
      std::string::size_type repPos = manifest_url_.find("$START_NUMBER$");
      if (repPos != std::string::npos)
      {
        while (repPos && manifest_url_[repPos] != '&' && manifest_url_[repPos] != '?')--repPos;
        if (repPos)
        {
          update_parameter_ = manifest_url_.substr(repPos);
          manifest_url_.resize(manifest_url_.size() - update_parameter_.size());
        }
        else
        {
          Log(LOGLEVEL_ERROR, "Cannot find update parameter delimiter (%s)", manifest_url_.c_str());
        }
      }
    }
    else
      update_parameter_ = manifestUpdateParam;
  }

  std::string AdaptiveTree::BuildDownloadUrl(const std::string& url) const
  {
    if (!url.empty())
    {
      if (url.front() != '/' && url.find("://") == std::string::npos)
        return base_url_ + url;
      else if (url.front() == '/')
        return base_domain_ + url;
    }
    return url;
  }

  void AdaptiveTree::SortTree()
  {
    for (std::vector<Period*>::const_iterator bp(periods_.begin()), ep(periods_.end()); bp != ep; ++bp)
    {
      std::stable_sort((*bp)->adaptationSets_.begin(), (*bp)->adaptationSets_.end(), AdaptationSet::compare);

      // Merge AUDIO streams, some provider pass everythng in own Audio sets
      for (std::vector<AdaptationSet*>::iterator ba((*bp)->adaptationSets_.begin()), ea((*bp)->adaptationSets_.end()); ba != ea;)
      {
        if ((*ba)->type_ == AUDIO && ba + 1 != ea && AdaptationSet::mergeable(*ba, *(ba + 1)))
        {
          for (size_t i(1); i < (*bp)->psshSets_.size(); ++i)
            if ((*bp)->psshSets_[i].adaptation_set_ == *ba)
              (*bp)->psshSets_[i].adaptation_set_ = *(ba + 1);

          (*(ba + 1))->representations_.insert((*(ba + 1))->representations_.end(), (*ba)->representations_.begin(), (*ba)->representations_.end());
          (*ba)->representations_.clear();
          ba = (*bp)->adaptationSets_.erase(ba);
          ea = (*bp)->adaptationSets_.end();
        }
        else
          ++ba;
      }

      for (std::vector<AdaptationSet*>::const_iterator ba((*bp)->adaptationSets_.begin()), ea((*bp)->adaptationSets_.end()); ba != ea; ++ba)
      {
        std::sort((*ba)->representations_.begin(), (*ba)->representations_.end(), Representation::compare);
        for (std::vector<Representation*>::iterator br((*ba)->representations_.begin()), er((*ba)->representations_.end()); br != er; ++br)
          (*br)->SetScaling();
      }
    }
  }

  void AdaptiveTree::RefreshUpdateThread()
  {
    if (HasUpdateThread())
    {
      std::lock_guard<std::mutex> lck(updateMutex_);
      updateVar_.notify_one();
    }
  }

  void AdaptiveTree::StartUpdateThread()
  {
    if (!updateThread_ && ~updateInterval_ && has_timeshift_buffer_ && !update_parameter_.empty())
      updateThread_ = new std::thread(&AdaptiveTree::SegmentUpdateWorker, this);
  }

  void AdaptiveTree::SegmentUpdateWorker()
  {
    std::unique_lock<std::mutex> updLck(updateMutex_);
    while (~updateInterval_ && has_timeshift_buffer_)
    {
      if (updateVar_.wait_for(updLck, std::chrono::milliseconds(updateInterval_)) == std::cv_status::timeout)
      {
        std::lock_guard<std::mutex> lck(treeMutex_);
        lastUpdated_ = std::chrono::system_clock::now();
        RefreshLiveSegments();
      }
    }
  }
} // namespace
