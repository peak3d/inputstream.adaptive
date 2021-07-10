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

#include "AdaptiveStream.h"

#include "../log.h"
#include "../oscompat.h"

#include <algorithm>
#include <cstring>
#include <iostream>

using namespace adaptive;

const size_t AdaptiveStream::MAXSEGMENTBUFFER = 10;

AdaptiveStream::AdaptiveStream(AdaptiveTree& tree,
                               AdaptiveTree::AdaptationSet* adp,
                               const std::map<std::string, std::string>& media_headers,
                               bool play_timeshift_buffer,
                               size_t repId)
  : thread_data_(nullptr),
    tree_(tree),
    observer_(nullptr),
    current_period_(tree_.current_period_),
    current_adp_(adp),
    current_rep_(repId ? adp->representations_[adp->representations_.size() - repId]
                       : tree.ChooseRepresentation(adp)),
    available_segment_buffers_(0),
    valid_segment_buffers_(0),
    media_headers_(media_headers),
    segment_read_pos_(0),
    currentPTSOffset_(0),
    absolutePTSOffset_(0),
    lastUpdated_(std::chrono::system_clock::now()),
    m_fixateInitialization(false),
    m_segmentFileOffset(0),
    play_timeshift_buffer_(play_timeshift_buffer)
{
  segment_buffers_.resize(MAXSEGMENTBUFFER);
}

AdaptiveStream::~AdaptiveStream()
{
  stop();
  clear();

  for (SEGMENTBUFFER& buf : segment_buffers_)
    delete[] buf.segment.url;
}

void AdaptiveStream::ResetSegment(const AdaptiveTree::Segment* segment)
{
  segment_read_pos_ = 0;

  if (segment && !(current_rep_->flags_ & (AdaptiveTree::Representation::SEGMENTBASE |
                                           AdaptiveTree::Representation::TEMPLATE |
                                           AdaptiveTree::Representation::URLSEGMENTS)))
    absolute_position_ = segment->range_begin_;
}

void AdaptiveStream::ResetActiveBuffer(bool oneValid)
{
  valid_segment_buffers_ = oneValid ? 1 : 0;
  available_segment_buffers_ = valid_segment_buffers_;
  absolute_position_ = 0;
  segment_buffers_[0].buffer.clear();
  segment_read_pos_ = 0;
}

// Make sure worker is in CV wait state.
void AdaptiveStream::StopWorker()
{
  // stop downloading chunks
  stopped_ = true;
  // wait until last reading operation stopped
  // make sure download section in worker thread is done.
  std::unique_lock<std::mutex> lckrw(thread_data_->mutex_rw_);
  while (worker_processing_)
    thread_data_->signal_rw_.wait(lckrw);
  // Now the worker thread should keep the lock until it starts waiting
  // to get CV signaled - make sure we are at this point.
  std::lock_guard<std::mutex> lckdl(thread_data_->mutex_dl_);
  // Make sure that worker continues at next notify
  stopped_ = false;
}

bool AdaptiveStream::download_segment()
{
  if (download_url_.empty())
    return false;

  return download(download_url_.c_str(), download_headers_);
}

void AdaptiveStream::worker()
{
  std::unique_lock<std::mutex> lckdl(thread_data_->mutex_dl_);
  worker_processing_ = false;
  thread_data_->signal_dl_.notify_one();
  do
  {
    while (!thread_data_->thread_stop_ &&
           (stopped_ || valid_segment_buffers_ >= available_segment_buffers_))
      thread_data_->signal_dl_.wait(lckdl);

    if (!thread_data_->thread_stop_)
    {
      worker_processing_ = true;

      prepareDownload();

      // tell the main thread that we have processed prepare_download;
      thread_data_->signal_dl_.notify_one();
      lckdl.unlock();

      bool ret(download_segment());
      unsigned int retryCount(10);

      //Some streaming software offers subtitle tracks with missing fragments, usually live tv
      //When a programme is broadcasted that has subtitles, subtitles fragments are offered
      //TODO: Ensure we continue with the next segment after one retry on errors
      if (current_adp_->type_ == AdaptiveTree::SUBTITLE)
        retryCount = 1;

      while (!ret && !stopped_ && retryCount-- && tree_.has_timeshift_buffer_)
      {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        Log(LOGLEVEL_DEBUG, "AdaptiveStream: trying to reload segment ...");
        ret = download_segment();
      }

      lckdl.lock();

      //Signal finished download
      {
        std::lock_guard<std::mutex> lckrw(thread_data_->mutex_rw_);
        download_url_.clear();
        if (!ret)
          stopped_ = true;
      }
      worker_processing_ = false;

      thread_data_->signal_rw_.notify_one();
    }
  } while (!thread_data_->thread_stop_);
}

int AdaptiveStream::SecondsSinceUpdate() const
{
  const std::chrono::time_point<std::chrono::system_clock>& tPoint(
      lastUpdated_ > tree_.GetLastUpdated() ? lastUpdated_ : tree_.GetLastUpdated());
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - tPoint)
          .count());
}

bool AdaptiveStream::write_data(const void* buffer, size_t buffer_size)
{
  {
    std::lock_guard<std::mutex> lckrw(thread_data_->mutex_rw_);

    if (stopped_)
      return false;

    // we write always into the last active segment
    std::string& segment_buffer = segment_buffers_[valid_segment_buffers_ - 1].buffer;

    size_t insertPos(segment_buffer.size());
    segment_buffer.resize(insertPos + buffer_size);
    tree_.OnDataArrived(download_segNum_, download_pssh_set_, m_iv,
                        reinterpret_cast<const uint8_t*>(buffer),
                        reinterpret_cast<uint8_t*>(&segment_buffer[0]), insertPos, buffer_size);
  }
  thread_data_->signal_rw_.notify_one();
  return true;
}

bool AdaptiveStream::start_stream()
{
  if (!current_rep_)
    return false;

  if (!thread_data_)
  {
    thread_data_ = new THREADDATA();
    std::unique_lock<std::mutex> lckdl(thread_data_->mutex_dl_);
    thread_data_->Start(this);
    // Wait until worker thread is waiting for input
    thread_data_->signal_dl_.wait(lckdl);
  }

  {
    std::unique_lock<std::mutex> lckrw(thread_data_->mutex_rw_);
    stopped_ = !ResolveSegmentBase(current_rep_, true);
    if (stopped_)
      return false;
  }

  if (!play_timeshift_buffer_ && tree_.has_timeshift_buffer_ &&
      current_rep_->segments_.data.size() > 1 && tree_.periods_.size() == 1)
  {
    std::int32_t pos;
    if (tree_.has_timeshift_buffer_ || tree_.available_time_ >= tree_.stream_start_)
      pos = static_cast<int32_t>(current_rep_->segments_.data.size() - 1);
    else
    {
      pos = static_cast<int32_t>(
          ((tree_.stream_start_ - tree_.available_time_) * current_rep_->timescale_) /
          current_rep_->duration_);
      if (!pos)
        pos = 1;
    }
    //go at least 12 secs back
    uint64_t duration(current_rep_->get_segment(pos)->startPTS_ -
                      current_rep_->get_segment(pos - 1)->startPTS_);
    pos -= static_cast<uint32_t>((12 * current_rep_->timescale_) / duration) + 1;
    current_rep_->current_segment_ = current_rep_->get_segment(pos < 0 ? 0 : pos);
  }
  else
    current_rep_->current_segment_ = nullptr; // start from beginning

  const AdaptiveTree::Segment* next_segment =
      current_rep_->get_next_segment(current_rep_->current_segment_);

  if (!next_segment)
  {
    absolute_position_ = ~0;
    stopped_ = true;
    return true;
  }

  stopped_ = false;
  absolute_position_ = 0;

  // load the initialization segment
  const AdaptiveTree::Segment* loadingSeg = current_rep_->get_initialization();
  if (loadingSeg)
  {
    StopWorker();

    // create a segment_buffer at pos 0
    std::move(segment_buffers_.begin(), segment_buffers_.begin() + available_segment_buffers_,
              segment_buffers_.begin() + 1);
    segment_buffers_[0].segment.url = nullptr;
    ++available_segment_buffers_;

    segment_buffers_[0].segment.Copy(loadingSeg);
    segment_buffers_[0].rep = current_rep_;
    segment_buffers_[0].segment_number = ~0U;
    segment_buffers_[0].buffer.clear();
    segment_read_pos_ = 0;

    // Force writing the data into segment_buffers_[0]
    // Store the # of valid buffers so we can resore later
    size_t valid_segment_buffers = valid_segment_buffers_;
    valid_segment_buffers_ = 0;

    if (!prepareDownload() || !download_segment())
      stopped_ = true;

    valid_segment_buffers_ = valid_segment_buffers + 1;
  }

  if (!stopped_)
  {
    const_cast<adaptive::AdaptiveTree::Representation*>(current_rep_)->flags_ |=
        adaptive::AdaptiveTree::Representation::ENABLED;
    return false;
  }
  return true;
}

void AdaptiveStream::ReplacePlaceholder(std::string& url, const std::string placeholder, uint64_t value)
{
  std::string::size_type lenReplace(placeholder.length());
  std::string::size_type np(url.find(placeholder));
  char rangebuf[128];

  if (np == std::string::npos)
    return;

  np += lenReplace;

  std::string::size_type npe(url.find('$', np));

  char fmt[16];
  if (np == npe)
    strcpy(fmt, "%" PRIu64);
  else
    strcpy(fmt, url.substr(np, npe - np).c_str());

  sprintf(rangebuf, fmt, value);
  url.replace(np - lenReplace, npe - np + lenReplace + 1, rangebuf);
}

bool AdaptiveStream::prepareDownload()
{
  // We assume, that we find the next segment to load in the next valid_segment_buffers_
  if (valid_segment_buffers_ >= available_segment_buffers_)
    return false;

  const AdaptiveTree::Representation* rep = segment_buffers_[valid_segment_buffers_].rep;
  const AdaptiveTree::Segment* seg = &segment_buffers_[valid_segment_buffers_].segment;
  // segNum == ~0U is initialization segment!
  unsigned int segNum = segment_buffers_[valid_segment_buffers_].segment_number;
  segment_buffers_[valid_segment_buffers_].buffer.clear();
  ++valid_segment_buffers_;

  if (!seg)
    return false;

  char rangebuf[128], *rangeHeader(0);

  if (!(rep->flags_ & AdaptiveTree::Representation::SEGMENTBASE))
  {
    if (!(rep->flags_ & AdaptiveTree::Representation::TEMPLATE))
    {
      if (rep->flags_ & AdaptiveTree::Representation::URLSEGMENTS)
      {
        download_url_ = seg->url;
        if (download_url_.find("://") == std::string::npos)
          download_url_ = rep->url_ + download_url_;
      }
      else
        download_url_ = rep->url_;
      if (~seg->range_begin_)
      {
        uint64_t fileOffset = ~segNum ? m_segmentFileOffset : 0;
        if (~seg->range_end_)
          sprintf(rangebuf, "bytes=%" PRIu64 "-%" PRIu64, seg->range_begin_ + fileOffset,
                  seg->range_end_ + fileOffset);
        else
          sprintf(rangebuf, "bytes=%" PRIu64 "-", seg->range_begin_ + fileOffset);
        rangeHeader = rangebuf;
      }
    }
    else if (~segNum) //templated segment
    {
      download_url_ = rep->segtpl_.media;
      ReplacePlaceholder(download_url_, "$Number", seg->range_end_);
      ReplacePlaceholder(download_url_, "$Time", seg->range_begin_);
    }
    else //templated initialization segment
      download_url_ = rep->url_;
  }
  else
  {
    if (rep->flags_ & AdaptiveTree::Representation::TEMPLATE && ~segNum)
    {
      download_url_ = rep->segtpl_.media;
      ReplacePlaceholder(download_url_, "$Number", rep->startNumber_);
      ReplacePlaceholder(download_url_, "$Time", 0);
    }
    else
      download_url_ = rep->url_;
    if (~seg->range_begin_)
    {
      uint64_t fileOffset = ~segNum ? m_segmentFileOffset : 0;
      if (~seg->range_end_)
        sprintf(rangebuf, "bytes=%" PRIu64 "-%" PRIu64, seg->range_begin_ + fileOffset,
                seg->range_end_ + fileOffset);
      else
        sprintf(rangebuf, "bytes=%" PRIu64 "-", seg->range_begin_ + fileOffset);
      rangeHeader = rangebuf;
    }
  }

  download_segNum_ = segNum;
  download_pssh_set_ = seg->pssh_set_;
  download_headers_ = media_headers_;
  if (rangeHeader)
    download_headers_["Range"] = rangeHeader;
  else
    download_headers_.erase("Range");

  download_url_ = tree_.BuildDownloadUrl(download_url_);

  return true;
}

bool AdaptiveStream::ensureSegment()
{
  if (stopped_)
    return false;

  // We an only switch to the next segment, if the current (== segment_buffers_[0]) is finished.
  // This is the case if we have more than 1 valid segments, or worker is not processing anymore.
  if ((!worker_processing_ || valid_segment_buffers_ > 1) &&
      segment_read_pos_ >= segment_buffers_[0].buffer.size())
  {
    // wait until worker is ready for new segment
    std::unique_lock<std::mutex> lck(thread_data_->mutex_dl_);
    // lock live segment updates
    std::lock_guard<std::mutex> lckTree(tree_.GetTreeMutex());

    if (tree_.HasUpdateThread() && SecondsSinceUpdate() > 1)
    {
      tree_.RefreshSegments(current_period_, current_adp_, current_rep_, current_adp_->type_);
      lastUpdated_ = std::chrono::system_clock::now();
    }

    if (m_fixateInitialization)
      return false;

    if (valid_segment_buffers_)
    {
      // rotate element 0 to the end
      std::rotate(segment_buffers_.begin(), segment_buffers_.begin() + 1,
                  segment_buffers_.begin() + available_segment_buffers_);
      --valid_segment_buffers_;
      --available_segment_buffers_;
    }

    // Check for representation switch here
    //const_cast<adaptive::AdaptiveTree::Representation*>(current_rep_)->flags_ &=
    //    ~adaptive::AdaptiveTree::Representation::ENABLED;
    //   if (observer_)
    // observer_->OnStreamChange(this);


    // Set the future segment for all not downloaded segments
    // Note that segment pointers can be changed during Live-Update
    // TODO:: This is maybe a good place to do handle bitstream changes (???)
    const AdaptiveTree::Segment* nextSegment =
        current_rep_->get_next_segment(current_rep_->current_segment_);

    if (nextSegment)
    {
      uint32_t nextsegmentPos = current_rep_->get_segment_pos(nextSegment);

      const AdaptiveTree::Representation* newRep = tree_.ChooseRepresentation(current_adp_);
      ResolveSegmentBase(newRep, false);

      for (size_t updPos(available_segment_buffers_); updPos < 5 /* TODO */; ++updPos)
      {
        const AdaptiveTree::Segment* futureSegment = newRep->get_segment(nextsegmentPos + updPos);

        if (futureSegment)
        {
          segment_buffers_[updPos].segment.Copy(futureSegment);
          segment_buffers_[updPos].segment_number = newRep->startNumber_ + nextsegmentPos + updPos;
          segment_buffers_[updPos].rep = newRep;
          ++available_segment_buffers_;
        }
        else
          break;
      }

      current_rep_->current_segment_ = nextSegment;
      ResetSegment(nextSegment);

      currentPTSOffset_ =
          (nextSegment->startPTS_ * current_rep_->timescale_ext_) / current_rep_->timescale_int_;

      absolutePTSOffset_ = (current_rep_->segments_[0]->startPTS_ * current_rep_->timescale_ext_) /
                           current_rep_->timescale_int_;

      if (observer_ && nextSegment != &current_rep_->initialization_ && ~nextSegment->startPTS_)
        observer_->OnSegmentChanged(this);

      thread_data_->signal_dl_.notify_one();
      // Make sure that we have at least one segment filling
      // Otherwise we lead into a deadlock because first condition is false.
      if (!valid_segment_buffers_)
        thread_data_->signal_dl_.wait(lck);
    }
    else if (tree_.HasUpdateThread() && current_period_ == tree_.periods_.back())
    {
      current_rep_->flags_ |= AdaptiveTree::Representation::WAITFORSEGMENT;
      Log(LOGLEVEL_DEBUG, "Begin WaitForSegment stream %s", current_rep_->id.c_str());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return false;
    }
    else
    {
      stopped_ = true;
      return false;
    }
  }
  return true;
}


uint32_t AdaptiveStream::read(void* buffer, uint32_t bytesToRead)
{
  if (stopped_)
    return false;

  std::unique_lock<std::mutex> lckrw(thread_data_->mutex_rw_);

NEXTSEGMENT:
  if (ensureSegment() && bytesToRead)
  {
    while (true)
    {
      uint32_t avail = segment_buffers_[0].buffer.size() - segment_read_pos_;
      if (avail < bytesToRead && worker_processing_)
      {
        thread_data_->signal_rw_.wait(lckrw);
        continue;
      }

      if (avail > bytesToRead)
        avail = bytesToRead;

      segment_read_pos_ += avail;
      absolute_position_ += avail;

      if (avail == bytesToRead)
      {
        memcpy(buffer, segment_buffers_[0].buffer.data() + (segment_read_pos_ - avail), avail);
        return avail;
      }
      // If we call read after the last chunk was read but before worker finishes download, we end up here.
      if (!avail)
        goto NEXTSEGMENT;
      return 0;
    }
  }
  return 0;
}

bool AdaptiveStream::seek(uint64_t const pos)
{
  if (stopped_)
    return false;

  std::unique_lock<std::mutex> lckrw(thread_data_->mutex_rw_);

  // we seek only in the current segment
  if (!stopped_ && pos >= absolute_position_ - segment_read_pos_)
  {
    segment_read_pos_ = static_cast<uint32_t>(pos - (absolute_position_ - segment_read_pos_));

    while (segment_read_pos_ > segment_buffers_[0].buffer.size() && worker_processing_)
      thread_data_->signal_rw_.wait(lckrw);

    if (segment_read_pos_ > segment_buffers_[0].buffer.size())
    {
      segment_read_pos_ = static_cast<uint32_t>(segment_buffers_[0].buffer.size());
      return false;
    }
    absolute_position_ = pos;
    return true;
  }
  return false;
}

bool AdaptiveStream::getSize(unsigned long long& sz)
{
  if (stopped_)
    return false;

  std::unique_lock<std::mutex> lckrw(thread_data_->mutex_rw_);

  if (ensureSegment())
  {
    while (true)
    {
      if (worker_processing_)
      {
        thread_data_->signal_rw_.wait(lckrw);
        continue;
      }
      sz = segment_buffers_[0].buffer.size();
      return true;
    }
  }
  return false;
}

uint64_t AdaptiveStream::getMaxTimeMs()
{
  if (current_rep_->flags_ & AdaptiveTree::Representation::SUBTITLESTREAM)
    return 0;

  if (current_rep_->segments_.empty())
    return 0;

  uint64_t duration =
      current_rep_->segments_.size() > 1
          ? current_rep_->segments_[current_rep_->segments_.size() - 1]->startPTS_ -
                current_rep_->segments_[current_rep_->segments_.size() - 2]->startPTS_
          : 0;

  uint64_t timeExt =
      ((current_rep_->segments_[current_rep_->segments_.size() - 1]->startPTS_ + duration) *
       current_rep_->timescale_ext_) /
      current_rep_->timescale_int_;

  return (timeExt - absolutePTSOffset_) / 1000;
}

bool AdaptiveStream::seek_time(double seek_seconds, bool preceeding, bool& needReset)
{
  if (!current_rep_)
    return false;

  if (stopped_)
    // For subtitles which come in one file we should return true!
    return current_rep_->segments_.empty();

  if (current_rep_->flags_ & AdaptiveTree::Representation::SUBTITLESTREAM)
    return true;

  std::unique_lock<std::mutex> lckTree(tree_.GetTreeMutex());

  uint32_t choosen_seg(~0);

  uint64_t sec_in_ts = static_cast<uint64_t>(seek_seconds * current_rep_->timescale_);
  choosen_seg = 0; //Skip initialization
  while (choosen_seg < current_rep_->segments_.data.size() &&
         sec_in_ts > current_rep_->get_segment(choosen_seg)->startPTS_)
    ++choosen_seg;

  if (choosen_seg == current_rep_->segments_.data.size())
  {
    if (sec_in_ts < current_rep_->segments_[0]->startPTS_ + current_rep_->duration_)
      --choosen_seg;
    else
      return false;
  }

  if (choosen_seg && current_rep_->get_segment(choosen_seg)->startPTS_ > sec_in_ts)
    --choosen_seg;

  // Never seek into expired segments.....
  if (choosen_seg < current_rep_->expired_segments_)
    choosen_seg = current_rep_->expired_segments_;

  if (!preceeding && sec_in_ts > current_rep_->get_segment(choosen_seg)->startPTS_ &&
      current_adp_->type_ ==
          AdaptiveTree::VIDEO) //Assume that we have I-Frames only at segment start
    ++choosen_seg;

  const AdaptiveTree::Segment *old_seg(current_rep_->current_segment_),
      *newSeg(current_rep_->get_segment(choosen_seg));
  if (newSeg)
  {
    needReset = true;
    if (newSeg != old_seg)
    {
      StopWorker();
      // EnsureSegment loads always the next segment, so go back 1
      current_rep_->current_segment_ =
          current_rep_->get_segment(current_rep_->get_segment_pos(newSeg) - 1);
      // TODO: if new segment is already prefetched, don't ResetActiveBuffer;
      ResetActiveBuffer(false);
    }
    else if (!preceeding)
    {
      absolute_position_ -= segment_read_pos_;
      segment_read_pos_ = 0;
    }
    else
      needReset = false;
    return true;
  }
  else
    current_rep_->current_segment_ = old_seg;
  return false;
}

bool AdaptiveStream::waitingForSegment(bool checkTime) const
{
  if (tree_.HasUpdateThread())
  {
    std::lock_guard<std::mutex> lckTree(tree_.GetTreeMutex());
    if (current_rep_ && (current_rep_->flags_ & AdaptiveTree::Representation::WAITFORSEGMENT) != 0)
      return !checkTime ||
             (current_adp_->type_ != AdaptiveTree::VIDEO &&
              current_adp_->type_ != AdaptiveTree::AUDIO) ||
             SecondsSinceUpdate() < 1;
  }
  return false;
}

void AdaptiveStream::FixateInitialization(bool on)
{
  m_fixateInitialization = on && current_rep_->get_initialization() != nullptr;
}

bool AdaptiveStream::ResolveSegmentBase(const AdaptiveTree::Representation* rep, bool stopWorker)
{
  stopped_ = false;
  /* If we have indexRangeExact SegmentBase, update SegmentList from SIDX */
  if (rep->flags_ & AdaptiveTree::Representation::SEGMENTBASE)
  {
    // Make sure the worker thread is in idle
    // Asssumtion: mutex_rw is already locked
    if (stopWorker)
      StopWorker();

    //We use the first segment as working buffer;
    std::move(segment_buffers_.begin(), segment_buffers_.begin() + available_segment_buffers_,
              segment_buffers_.begin() + 1);

    size_t valid_segment_buffers = valid_segment_buffers_;
    size_t available_segment_buffers = available_segment_buffers_;
    valid_segment_buffers_ = 0;
    available_segment_buffers_ = 1;

    SEGMENTBUFFER& sb(segment_buffers_[0]);
    sb.rep = rep;
    sb.segment.url = nullptr;
    sb.segment_number = 0;
    sb.buffer.clear();
    segment_read_pos_ = 0;

    if (rep->indexRangeMin_ || !(rep->get_initialization()))
    {
      sb.segment.range_begin_ = rep->indexRangeMin_;
      sb.segment.range_end_ = rep->indexRangeMax_;
      sb.segment.startPTS_ = ~0ULL;
      sb.segment.pssh_set_ = 0;
    }
    else if (rep->get_initialization())
      sb.segment = *rep->get_initialization();
    else
      available_segment_buffers_ = 0;

    absolute_position_ = 0;
    m_fixateInitialization = true;
    if (prepareDownload() && download_segment() && parseIndexRange())
      const_cast<AdaptiveTree::Representation*>(rep)->flags_ &=
          ~AdaptiveTree::Representation::SEGMENTBASE;
    else
      stopped_ = true;
    m_fixateInitialization = false;

    // restore segment_buffers_
    //We use the first segment as working buffer;
    valid_segment_buffers_ = valid_segment_buffers;
    available_segment_buffers_ = available_segment_buffers;

    if (available_segment_buffers_)
      std::move(segment_buffers_.begin() + 1, segment_buffers_.begin() + available_segment_buffers_,
                segment_buffers_.begin());
  }
  return !stopped_;
}

void AdaptiveStream::info(std::ostream& s)
{
  static const char* ts[4] = {"NoType", "Video", "Audio", "Text"};
  s << ts[current_adp_->type_]
    << " representation: " << current_rep_->url_.substr(current_rep_->url_.find_last_of('/') + 1)
    << " bandwidth: " << current_rep_->bandwidth_ << std::endl;
}

void AdaptiveStream::stop()
{
  stopped_ = true;
  if (current_rep_)
    const_cast<adaptive::AdaptiveTree::Representation*>(current_rep_)->flags_ &=
        ~adaptive::AdaptiveTree::Representation::ENABLED;
  if (thread_data_)
  {
    StopWorker();
    delete thread_data_;
    thread_data_ = nullptr;
  }
};

void AdaptiveStream::clear()
{
  current_adp_ = 0;
  current_rep_ = 0;
}
