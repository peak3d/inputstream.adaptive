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

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <kodi/AddonBase.h>

namespace adaptive
{
  class AdaptiveStream;

  class ATTRIBUTE_HIDDEN AdaptiveStreamObserver
  {
  public:
    virtual void OnSegmentChanged(AdaptiveStream *stream) = 0;
    virtual void OnStreamChange(AdaptiveStream *stream) = 0;
  };

  class ATTRIBUTE_HIDDEN AdaptiveStream
  {
  public:
    AdaptiveStream(AdaptiveTree &tree, AdaptiveTree::StreamType type);
    virtual ~AdaptiveStream();
    void set_observer(AdaptiveStreamObserver *observer){ observer_ = observer; };
    bool prepare_stream(AdaptiveTree::AdaptationSet* adp,
                        const uint32_t width,
                        const uint32_t height,
                        uint32_t hdcpLimit,
                        uint16_t hdcpVersion,
                        uint32_t min_bandwidth,
                        uint32_t max_bandwidth,
                        unsigned int repId,
                        const std::map<std::string, std::string>& media_headers);
    bool start_stream(const uint32_t seg_offset, uint16_t width, uint16_t height, bool play_timeshift_buffer);
    bool restart_stream();
    bool select_stream(bool force = false, bool justInit = false, unsigned int repId = 0);
    void stop();
    void clear();
    void info(std::ostream &s);
    unsigned int getWidth() const { return width_; };
    unsigned int getHeight() const { return height_; };
    unsigned int getBandwidth() const { return bandwidth_; };
    uint64_t getMaxTimeMs();

    unsigned int get_type()const{ return type_; };

    bool ensureSegment();
    uint32_t read(void* buffer, uint32_t  bytesToRead);
    uint64_t tell(){ read(0, 0);  return absolute_position_; };
    bool seek(uint64_t const pos);
    bool getSize(unsigned long long& sz);
    bool seek_time(double seek_seconds, bool preceeding, bool &needReset);
    AdaptiveTree::Period* getPeriod() { return current_period_; };
    AdaptiveTree::AdaptationSet* getAdaptationSet() { return current_adp_; };
    AdaptiveTree::Representation* getRepresentation() { return current_rep_; };
    double get_download_speed() const { return tree_.get_download_speed(); };
    void set_download_speed(double speed) { tree_.set_download_speed(speed); };
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
    bool prepareDownload(const AdaptiveTree::Segment *seg);
    adaptive::AdaptiveTree& GetTree() { return tree_; };
    virtual void SetLastUpdated(std::chrono::system_clock::time_point tm){};
    std::chrono::time_point<std::chrono::system_clock> lastUpdated_;

  private:
    // Segment download section
    void ResetSegment();
    bool download_segment();
    void worker();
    int SecondsSinceUpdate() const;
    static void ReplacePlaceholder(std::string &url, const std::string placeholder, uint64_t value);

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
        download_thread_.join();
      };

      std::mutex mutex_rw_, mutex_dl_;
      std::condition_variable signal_rw_, signal_dl_;
      std::thread download_thread_;
      bool thread_stop_;
    };
    THREADDATA *thread_data_;

    AdaptiveTree &tree_;
    AdaptiveTree::StreamType type_;
    AdaptiveStreamObserver *observer_;
    // Active configuration
    AdaptiveTree::Period* current_period_;
    AdaptiveTree::AdaptationSet* current_adp_;
    AdaptiveTree::Representation *current_rep_;
    std::string download_url_;
    //We assume that a single segment can build complete frames
    std::string segment_buffer_;
    std::map<std::string, std::string> media_headers_, download_headers_;
    std::size_t segment_read_pos_;
    uint64_t absolute_position_;
    uint64_t currentPTSOffset_, absolutePTSOffset_;

    uint16_t width_, height_;
    uint32_t bandwidth_;
    uint32_t hdcpLimit_;
    uint16_t hdcpVersion_;
    uint16_t download_pssh_set_;
    unsigned int download_segNum_;
    bool stopped_;
    uint8_t m_iv[16];
    bool m_fixateInitialization;
    uint64_t m_segmentFileOffset;
    bool play_timeshift_buffer_;
  };
};
