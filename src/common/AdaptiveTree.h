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

#include <vector>
#include <string>
#include <map>
#include <inttypes.h>
#include "expat.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace adaptive
{

template <typename T>
struct SPINCACHE
{
  SPINCACHE() :basePos(0) {};

  size_t basePos;

  const T *operator[](uint32_t pos) const
  {
    if (!~pos)
      return 0;
    size_t realPos = basePos + pos;
    if (realPos >= data.size())
    {
      realPos -= data.size();
      if (realPos == basePos)
        return 0;
    }
    return &data[realPos];
  };

  uint32_t pos(const T* elem) const
  {
    size_t realPos = elem - &data[0];
    if (realPos < basePos)
      realPos += data.size() - basePos;
    else
      realPos -= basePos;
    return static_cast<std::uint32_t>(realPos);
  };

  void insert(const T &elem)
  {
    data[basePos] = elem;
    ++basePos;
    if (basePos == data.size())
      basePos = 0;
  }

  void swap(SPINCACHE<T> &other)
  {
    data.swap(other.data);
    std::swap(basePos, other.basePos);
  }

  void clear()
  {
    data.clear();
    basePos = 0;
  }

  bool empty() const { return data.empty(); };

  size_t size() const { return data.size(); };

  std::vector<T> data;
};

class AdaptiveTree
{
public:
  enum StreamType
  {
    NOTYPE,
    VIDEO,
    AUDIO,
    SUBTITLE,
    STREAM_TYPE_COUNT
  };

  enum ContainerType : uint8_t
  {
    CONTAINERTYPE_NOTYPE,
    CONTAINERTYPE_INVALID,
    CONTAINERTYPE_MP4,
    CONTAINERTYPE_TS,
    CONTAINERTYPE_ADTS,
    CONTAINERTYPE_WEBM,
    CONTAINERTYPE_MATROSKA,
    CONTAINERTYPE_TEXT,
  };

  enum
  {
    ENCRYTIONSTATE_UNENCRYPTED = 0,
    ENCRYTIONSTATE_ENCRYPTED = 1,
    ENCRYTIONSTATE_SUPPORTED = 2
  };

  enum PREPARE_RESULT
  {
    PREPARE_RESULT_FAILURE,
    PREPARE_RESULT_OK,
    PREPARE_RESULT_DRMCHANGED,
  };

  // Node definition
  struct Segment
  {
    void SetRange(const char *range);
    void Copy(const Segment* src);
    uint64_t range_begin_ = 0; //Either byterange start or timestamp or ~0
    uint64_t range_end_ = 0; //Either byterange end or sequence_id if range_begin is ~0
    const char *url = nullptr;
    uint64_t startPTS_ = 0;
    uint16_t pssh_set_ = 0;
  };

  struct SegmentTemplate
  {
    SegmentTemplate() : timescale(0), duration(0), presentationTimeOffset(0){};
    std::string initialization;
    std::string media;
    unsigned int timescale, duration;
    uint64_t presentationTimeOffset;
  };

  struct Representation
  {
    Representation() :bandwidth_(0), samplingRate_(0), width_(0), height_(0), fpsRate_(0), fpsScale_(1), aspect_(0.0f),
      flags_(0), hdcpVersion_(0), indexRangeMin_(0), indexRangeMax_(0), channelCount_(0), nalLengthSize_(0), pssh_set_(0), expired_segments_(0),
      containerType_(AdaptiveTree::CONTAINERTYPE_MP4), startNumber_(1), ptsOffset_(0), nextPts_(0), duration_(0), timescale_(0), current_segment_(nullptr)
    {
      initialization_.range_begin_ = initialization_.range_end_ = ~0ULL;
      initialization_.url = nullptr;
    };
    void CopyBasicData(Representation* src);
    ~Representation() {
      if (flags_ & Representation::URLSEGMENTS)
      {
        for (std::vector<Segment>::iterator bs(segments_.data.begin()), es(segments_.data.end()); bs != es; ++bs)
          delete[] bs->url;
        if (flags_ & Representation::INITIALIZATION)
          delete[]initialization_.url;
      }
    };
    std::string url_;
    std::string id;
    std::string codecs_;
    std::string codec_private_data_;
    std::string source_url_;
    uint32_t bandwidth_;
    uint32_t samplingRate_;
    uint16_t width_, height_;
    uint32_t fpsRate_, fpsScale_;
    float aspect_;

    uint32_t assured_buffer_duration_;
    uint32_t max_buffer_duration_;
    //Flags
    static const uint16_t BYTERANGE = 0;
    static const uint16_t INDEXRANGEEXACT = 1;
    static const uint16_t TEMPLATE = 2;
    static const uint16_t TIMELINE = 4;
    static const uint16_t INITIALIZATION = 8;
    static const uint16_t SEGMENTBASE = 16;
    static const uint16_t SUBTITLESTREAM = 32;
    static const uint16_t INCLUDEDSTREAM = 64;
    static const uint16_t URLSEGMENTS = 128;
    static const uint16_t ENABLED = 256;
    static const uint16_t WAITFORSEGMENT = 512;
    static const uint16_t INITIALIZATION_PREFIXED = 1024;
    static const uint16_t DOWNLOADED = 2048;

    uint16_t flags_;
    uint16_t hdcpVersion_;

    uint32_t indexRangeMin_, indexRangeMax_;
    uint8_t channelCount_, nalLengthSize_;
    uint16_t pssh_set_;
    uint32_t expired_segments_;
    ContainerType containerType_;
    SegmentTemplate segtpl_;
    unsigned int startNumber_;
    uint64_t nextPts_;
    //SegmentList
    uint64_t ptsOffset_;
    uint64_t duration_;
    uint32_t timescale_;
    uint32_t timescale_ext_, timescale_int_;
    Segment initialization_;
    SPINCACHE<Segment> segments_;
    const Segment *current_segment_;
    const Segment *get_initialization()const { return (flags_ & INITIALIZATION) ? &initialization_ : 0; };
    const Segment *get_next_segment(const Segment *seg)const
    {
      if (!seg || seg == &initialization_)
        return segments_[0];
      else if (segments_.pos(seg) + 1 == segments_.data.size())
        return nullptr;
      else
        return segments_[segments_.pos(seg) + 1];
    };

    const Segment *get_segment(uint32_t pos)const
    {
      return ~pos ? segments_[pos] : nullptr;
    };

    const uint32_t get_segment_pos(const Segment *segment)const
    {
      return segment ? segments_.data.empty() ? 0 : segments_.pos(segment) : ~0;
    }

    const uint16_t get_psshset() const
    {
      return pssh_set_;
    }

    uint32_t getCurrentSegmentPos() const
    {
      return get_segment_pos(current_segment_);
    };

    uint32_t getCurrentSegmentNumber() const
    {
      return current_segment_ ? get_segment_pos(current_segment_) + startNumber_ : ~0U;
    };

    void SetScaling()
    {
      if (!timescale_)
      {
        timescale_ext_ = timescale_int_ = 1;
        return;
      }
      timescale_ext_ = 1000000;
      timescale_int_ = timescale_;
      while (timescale_ext_ > 1)
        if ((timescale_int_ / 10) * 10 == timescale_int_)
        {
          timescale_ext_ /= 10;
          timescale_int_ /= 10;
        }
        else
          break;
    }

    static bool compare(const Representation* a, const Representation *b) { return a->bandwidth_ < b->bandwidth_; };

  }*current_representation_;

  struct AdaptationSet
  {
    AdaptationSet() :type_(NOTYPE), timescale_(0), duration_(0), startPTS_(0), startNumber_(1), impaired_(false), original_(false), default_(false), forced_(false){ language_ = "unk"; };
    ~AdaptationSet() { for (std::vector<Representation* >::const_iterator b(representations_.begin()), e(representations_.end()); b != e; ++b) delete *b; };
    void CopyBasicData(AdaptationSet* src);
    StreamType type_;
    uint32_t timescale_;
    uint64_t duration_;
    uint64_t startPTS_;
    unsigned int startNumber_;
    bool impaired_, original_, default_, forced_;
    std::string language_;
    std::string mimeType_;
    std::string base_url_;
    std::string id_, group_;
    std::string codecs_;
    std::string audio_track_id_;
    std::string name_;
    std::vector<Representation*> representations_;
    SPINCACHE<uint32_t> segment_durations_;
    SegmentTemplate segtpl_;

    const uint32_t get_segment_duration(uint32_t pos)const
    {
      return *segment_durations_[pos];
    };

    static bool compare(const AdaptationSet* a, const AdaptationSet *b)
    {
      if (a->type_ != b->type_)
        return a->type_ < b->type_;
      if (a->language_ != b->language_)
        return a->language_ < b->language_;
      if (a->default_ != b->default_)
        return a->default_;

      if (a->type_ == AUDIO)
      {
        if (a->audio_track_id_ != b->audio_track_id_)
          return a->audio_track_id_ < b->audio_track_id_;

        if (a->name_ != b->name_)
          return a->name_ < b->name_;

        if (a->impaired_ != b->impaired_)
          return !a->impaired_;
        if (a->original_ != b->original_)
          return a->original_;

        if (a->representations_[0]->codecs_ != b->representations_[0]->codecs_)
          return a->representations_[0]->codecs_ < b->representations_[0]->codecs_;

        if (a->representations_[0]->channelCount_ != b->representations_[0]->channelCount_)
          return a->representations_[0]->channelCount_ < b->representations_[0]->channelCount_;
      }
      else if (a->type_ == SUBTITLE)
      {
        if (a->impaired_ != b->impaired_)
          return !a->impaired_;
        if (a->forced_ != b->forced_)
          return a->forced_;
      }

      return false;
    };

    static bool compareCodecs(const std::string &a, const std::string &b)
    {
      std::string::size_type posa = a.find_first_of('.');
      std::string::size_type posb = a.find_first_of('.');
      if (posa == posb)
        return a.compare(0, posa, b, 0, posb) == 0;
      return false;
    };

    static bool mergeable(const AdaptationSet* a, const AdaptationSet *b)
    {
      if (a->type_ == b->type_
        && a->timescale_ == b->timescale_
        && a->duration_ == b->duration_
        && a->startPTS_ == b->startPTS_
        && a->startNumber_ == b->startNumber_
        && a->impaired_ == b->impaired_
        && a->original_ == b->original_
        && a->default_ == b->default_
        && a->language_ == b->language_
        && a->mimeType_ == b->mimeType_
        && a->base_url_ == b->base_url_
        && a->audio_track_id_ == b->audio_track_id_
        && a->name_ == b->name_
        && a->id_ == b->id_
        && a->group_ == b->group_
        && compareCodecs(a->codecs_, b->codecs_))
      {
        return a->type_ == AUDIO
          && a->representations_[0]->channelCount_ == b->representations_[0]->channelCount_
          && compareCodecs(a->representations_[0]->codecs_, b->representations_[0]->codecs_);
      }
      return false;
    };
  }*current_adaptationset_;

  struct Period
  {
    struct PSSH
    {
      static const uint32_t MEDIA_VIDEO = 1;
      static const uint32_t MEDIA_AUDIO = 2;

      PSSH() :media_(0), use_count_(0) {};
      bool operator == (const PSSH &other) const { return !use_count_ || (media_ == other.media_ && pssh_ == other.pssh_ && defaultKID_ == other.defaultKID_ && iv == other.iv); };
      std::string pssh_;
      std::string defaultKID_;
      std::string iv;
      uint32_t media_;
      uint32_t use_count_;
      AdaptationSet *adaptation_set_;
    };
    std::vector<PSSH> psshSets_;

    Period() { psshSets_.push_back(PSSH()); };
    ~Period() { for (std::vector<AdaptationSet* >::const_iterator b(adaptationSets_.begin()), e(adaptationSets_.end()); b != e; ++b) delete *b; };
    void CopyBasicData(Period*);
    uint16_t InsertPSSHSet(PSSH* pssh);
    void InsertPSSHSet(uint16_t pssh_set) { ++psshSets_[pssh_set].use_count_; };
    void RemovePSSHSet(uint16_t pssh_set);

    std::vector<AdaptationSet*> adaptationSets_;
    std::string base_url_, id_;
    uint32_t timescale_ = 1000, startNumber_ = 1;
    uint64_t start_ = 0;
    uint64_t startPTS_ = 0;
    uint64_t duration_ = 0;
    unsigned int  encryptionState_ = ENCRYTIONSTATE_UNENCRYPTED;
    uint32_t included_types_ = 0;
    bool need_secure_decoder_ = false;
    SPINCACHE<uint32_t> segment_durations_;
    SegmentTemplate segtpl_;
  }*current_period_, *next_period_;

  struct RepresentationChooser
  {
    virtual Representation* ChooseRepresentation(AdaptationSet* adp) = 0;
  virtual Representation* ChooseNextRepresentation(AdaptationSet* adp , 
                                                Representation* rep,  
                                                size_t *available_segment_buffers_,
                                                size_t *valid_segment_buffers_,
                                                uint32_t *assured_buffer_length_,
                                                uint32_t * max_buffer_length_, 
                                                uint32_t rep_counter_) = 0;
  } *representation_chooser_ = nullptr;

  std::vector<Period*> periods_;
  std::string manifest_url_, base_url_, effective_url_, effective_filename_, base_domain_, update_parameter_;
  std::string::size_type update_parameter_pos_;
  std::string etag_, last_modified_;
  std::string media_renewal_url_;
  uint32_t media_renewal_time_;
  std::string manifest_parameter_;

  /* XML Parsing*/
  XML_Parser parser_;
  uint32_t currentNode_;
  uint32_t segcount_;
  uint64_t overallSeconds_, stream_start_, available_time_, publish_time_, base_time_;
  uint64_t minPresentationOffset;
  bool has_timeshift_buffer_, has_overall_seconds_;

  std::map<std::string, std::string> manifest_headers_;

  std::string supportedKeySystem_, location_;

  uint8_t adpChannelCount_, adp_pssh_set_;
  uint16_t adpwidth_, adpheight_;
  uint32_t adpfpsRate_;
  float adpaspect_;
  ContainerType adpContainerType_;
  bool adp_timelined_, period_timelined_;

  bool current_hasRepURN_, current_hasAdpURN_;
  std::string current_pssh_, current_defaultKID_, current_iv_;
  std::string license_url_;

  std::string strXMLText_;

  AdaptiveTree();
  virtual ~AdaptiveTree();

  virtual bool open(const std::string &url, const std::string &manifestUpdateParam) = 0;
  virtual PREPARE_RESULT prepareRepresentation(Period* period,
                                               AdaptationSet* adp,
                                               Representation* rep,
                                               bool update = false)
  {
    return PREPARE_RESULT_OK;
  };
  virtual void OnDataArrived(unsigned int segNum, uint16_t psshSet, uint8_t iv[16], const uint8_t *src, uint8_t *dst, size_t dstOffset, size_t dataSize);
  virtual void RefreshSegments(Period* period,
                               AdaptationSet* adp,
                               Representation* rep,
                               StreamType type){};

  bool has_type(StreamType t);
  void FreeSegments(Representation *rep);
  uint32_t estimate_segcount(uint64_t duration, uint32_t timescale);
  void SetFragmentDuration(const AdaptationSet* adp, const Representation* rep, size_t pos, uint64_t timestamp, uint32_t fragmentDuration, uint32_t movie_timescale);
  uint16_t insert_psshset(StreamType type, Period* period = nullptr, AdaptationSet* adp = nullptr);

  bool empty(){ return periods_.empty(); };
  AdaptationSet* GetAdaptationSet(unsigned int pos) const
  {
    return current_period_ && pos < current_period_->adaptationSets_.size()
               ? current_period_->adaptationSets_[pos]
               : 0;
  };
  std::mutex &GetTreeMutex() { return treeMutex_; };
  bool HasUpdateThread() const { return updateThread_ != 0 && has_timeshift_buffer_ && updateInterval_ && !update_parameter_.empty(); };
  void RefreshUpdateThread();
  const std::chrono::time_point<std::chrono::system_clock> GetLastUpdated() const { return lastUpdated_; };
  const std::chrono::time_point<std::chrono::system_clock> GetLastMediaRenewal() const { return lastMediaRenewal_; };

  Representation* ChooseRepresentation(AdaptationSet* adp)
  {
    return representation_chooser_ ? representation_chooser_->ChooseRepresentation(adp) : nullptr;
  };
  Representation* ChooseNextRepresentation(AdaptationSet* adp, 
                                      Representation* rep,  
                                      size_t *available_segment_buffers_,
                                      size_t *valid_segment_buffers_,
                                      uint32_t *assured_buffer_length_,
                                      uint32_t * max_buffer_length_, 
                                      uint32_t rep_counter_)
  {
    return representation_chooser_ ? representation_chooser_->ChooseNextRepresentation(adp,rep,available_segment_buffers_,
                                                                                      valid_segment_buffers_, assured_buffer_length_,
                                                                                      max_buffer_length_, rep_counter_) : nullptr;
  };

protected:
  virtual bool download(const char* url, const std::map<std::string, std::string> &manifestHeaders, void *opaque = nullptr, bool scanEffectiveURL = true);
  virtual bool write_data(void *buffer, size_t buffer_size, void *opaque) = 0;
  bool PreparePaths(const std::string &url, const std::string &manifestUpdateParam);
  void SortTree();

  // Live segment update section
  virtual void StartUpdateThread();
  virtual void RefreshLiveSegments(){};

  uint32_t updateInterval_;
  std::mutex treeMutex_, updateMutex_;
  std::condition_variable updateVar_;
  std::thread *updateThread_;
  std::chrono::time_point<std::chrono::system_clock> lastUpdated_;
  std::chrono::time_point<std::chrono::system_clock> lastMediaRenewal_;

private:
  void SegmentUpdateWorker();
};

}
