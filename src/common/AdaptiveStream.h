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

#include "AdaptiveTree.h"
#include <string>
#include <map>

#include <thread>
#include <mutex>
#include <condition_variable>

namespace adaptive
{
  class AdaptiveStream;

  class AdaptiveStreamObserver
  {
  public:
    virtual void OnSegmentChanged(AdaptiveStream *stream) = 0;
    virtual void OnStreamChange(AdaptiveStream *stream) = 0;
  };

  class AdaptiveStream
  {
  public:
    AdaptiveStream(AdaptiveTree& tree,
                   AdaptiveTree::AdaptationSet* adp,
                   const std::map<std::string, std::string>& media_headers,
                   bool play_timeshift_buffer,
                   size_t repId);
    virtual ~AdaptiveStream();
    void set_observer(AdaptiveStreamObserver *observer){ observer_ = observer; };
    bool start_stream();
    void stop();
    void clear();
    void info(std::ostream &s);
    uint64_t getMaxTimeMs();

    unsigned int get_type()const{ return current_adp_->type_; };

    bool ensureSegment();
    uint32_t read(void* buffer, uint32_t  bytesToRead);
    uint64_t tell(){ read(0, 0);  return absolute_position_; };
    bool seek(uint64_t const pos);
    bool getSize(unsigned long long& sz);
    bool seek_time(double seek_seconds, bool preceeding, bool &needReset);
    AdaptiveTree::Period* getPeriod() { return current_period_; };
    AdaptiveTree::AdaptationSet* getAdaptationSet() { return current_adp_; };
    AdaptiveTree::Representation* getRepresentation() { return current_rep_; };
    size_t getSegmentPos() { return current_rep_->getCurrentSegmentPos(); };
    uint64_t GetCurrentPTSOffset() { return currentPTSOffset_; };
    uint64_t GetAbsolutePTSOffset() { return absolutePTSOffset_; };
    bool waitingForSegment(bool checkTime = false) const;
    void FixateInitialization(bool on);
    void SetSegmentFileOffset(uint64_t offset) { m_segmentFileOffset = offset; };
  protected:
    virtual bool download(const char* url, const std::map<std::string, std::string> &mediaHeaders){ return false; };
    virtual bool parseIndexRange() { return false; };
    bool write_data(const void *buffer, size_t buffer_size);
    void setEffectiveURL(const std::string url) { tree_.effective_url_ = url; if (tree_.effective_url_.back() != '/') tree_.effective_url_ += '/'; };
    const std::string& getMediaRenewalUrl() const { return tree_.media_renewal_url_; };
    const uint32_t& getMediaRenewalTime() const { return tree_.media_renewal_time_; };
    std::string buildDownloadUrl(const std::string &url);
    uint32_t SecondsSinceMediaRenewal() const;
    void UpdateSecondsSinceMediaRenewal();

  private:
    // Segment download section
    void ResetSegment(const AdaptiveTree::Segment* segment);
    void ResetActiveBuffer(bool oneValid);
    void StopWorker();
    bool download_segment();
    void worker();
    bool prepareDownload();
    int SecondsSinceUpdate() const;
    static void ReplacePlacehoder(std::string &url, uint64_t index, uint64_t timeStamp);
    bool ResolveSegmentBase(const AdaptiveTree::Representation* rep, bool stopWorker);

    struct THREADDATA
    {
      THREADDATA()
        : thread_stop_(false)
      {
      }

      void Start(AdaptiveStream *parent)
      {
        download_thread_ = std::thread(&AdaptiveStream::worker, parent);
      }

      ~THREADDATA()
      {
        thread_stop_ = true;
        signal_dl_.notify_one();
        if (download_thread_.joinable())
          download_thread_.join();
      };

      std::mutex mutex_rw_, mutex_dl_;
      std::condition_variable signal_rw_, signal_dl_;
      std::thread download_thread_;
      bool thread_stop_;
    };
    THREADDATA *thread_data_;

    AdaptiveTree &tree_;
    AdaptiveStreamObserver *observer_;
    // Active configuration
    AdaptiveTree::Period* current_period_;
    AdaptiveTree::AdaptationSet* current_adp_;
    AdaptiveTree::Representation *current_rep_;
    std::string download_url_;

    static const size_t MAXSEGMENTBUFFER;
    struct SEGMENTBUFFER
    {
      std::string buffer;
      AdaptiveTree::Segment segment;
      unsigned int segment_number;
      const AdaptiveTree::Representation* rep;
    };
    std::vector<SEGMENTBUFFER> segment_buffers_;
    // number of segmentbuffers whith valid segment, always >= valid_segment_buffers_
    size_t available_segment_buffers_;
    // number of segment_buffers which are downloaded / downloading
    size_t valid_segment_buffers_;

    std::map<std::string, std::string> media_headers_, download_headers_;
    std::size_t segment_read_pos_;
    uint64_t absolute_position_;
    uint64_t currentPTSOffset_, absolutePTSOffset_;
    std::chrono::time_point<std::chrono::system_clock> lastUpdated_;
    std::chrono::time_point<std::chrono::system_clock> lastMediaRenewal_;

    uint16_t download_pssh_set_;
    unsigned int download_segNum_;
    bool stopped_, worker_processing_;
    uint8_t m_iv[16];
    bool m_fixateInitialization;
    uint64_t m_segmentFileOffset;
    bool play_timeshift_buffer_;
  };
};
