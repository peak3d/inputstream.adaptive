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

#include "SmoothTree.h"

#include "../helpers.h"
#include "../oscompat.h"
#include "PRProtectionParser.h"

#include <algorithm>
#include <cstring>
#include <float.h>
#include <fstream>
#include <string>

using namespace adaptive;

SmoothTree::SmoothTree()
{
  current_period_ = new AdaptiveTree::Period;
  periods_.push_back(current_period_);
}

/*----------------------------------------------------------------------
|   expat start
+---------------------------------------------------------------------*/
static void XMLCALL start(void* data, const char* el, const char** attr)
{
  SmoothTree* dash(reinterpret_cast<SmoothTree*>(data));

  if (dash->currentNode_ & SmoothTree::SSMNODE_SSM)
  {
    if (dash->currentNode_ & SmoothTree::SSMNODE_PROTECTION)
    {
      if (strcmp(el, "ProtectionHeader") == 0)
      {
        for (; *attr;)
        {
          if (strcmp((const char*)*attr, "SystemID") == 0)
          {
            if (strstr((const char*)*(attr + 1), "9A04F079-9840-4286-AB92-E65BE0885F95") ||
                strstr((const char*)*(attr + 1), "9a04f079-9840-4286-ab92-e65be0885f95"))
            {
              dash->strXMLText_.clear();
              dash->currentNode_ |=
                  SmoothTree::SSMNODE_PROTECTIONHEADER | SmoothTree::SSMNODE_PROTECTIONTEXT;
            }
            break;
          }
          attr += 2;
        }
      }
    }
    else if (dash->currentNode_ & SmoothTree::SSMNODE_STREAMINDEX)
    {
      if (strcmp(el, "QualityLevel") == 0)
      {
        //<QualityLevel Index = "0" Bitrate = "150000" NominalBitrate = "150784" BufferTime = "3000" FourCC = "AVC1" MaxWidth = "480" MaxHeight = "272" CodecPrivateData = "000000016742E01E96540F0477FE0110010ED100000300010000030032E4A0093401BB2F7BE3250049A00DD97BDF0A0000000168CE060CC8" NALUnitLengthField = "4" / >
        //<QualityLevel Index = "0" Bitrate = "48000" SamplingRate = "24000" Channels = "2" BitsPerSample = "16" PacketSize = "4" AudioTag = "255" FourCC = "AACH" CodecPrivateData = "131056E598" / >

        dash->current_representation_ = new SmoothTree::Representation();
        dash->current_representation_->url_ = dash->current_adaptationset_->base_url_;
        dash->current_representation_->timescale_ = dash->current_adaptationset_->timescale_;
        dash->current_representation_->flags_ |= AdaptiveTree::Representation::TEMPLATE;
        dash->current_representation_->segtpl_.media = dash->current_representation_->url_;

        std::string::size_type pos(
            dash->current_representation_->segtpl_.media.find("{start time}"));
        if (pos != std::string::npos)
          dash->current_representation_->segtpl_.media.replace(pos, 12, "$Time$");
        else
          return;

        pos = dash->current_representation_->segtpl_.media.find("{bitrate}");
        if (pos == std::string::npos)
          return;

        const char* bw = "0";
        const char* codecPrivateData = nullptr;

        for (; *attr;)
        {
          if (strcmp((const char*)*attr, "Bitrate") == 0)
            bw = (const char*)*(attr + 1);
          else if (strcmp((const char*)*attr, "FourCC") == 0)
          {
            dash->current_representation_->codecs_ = (const char*)*(attr + 1);
            std::transform(dash->current_representation_->codecs_.begin(),
                           dash->current_representation_->codecs_.end(),
                           dash->current_representation_->codecs_.begin(), ::tolower);
          }
          else if (strcmp((const char*)*attr, "MaxWidth") == 0)
            dash->current_representation_->width_ =
                static_cast<uint16_t>(atoi((const char*)*(attr + 1)));
          else if (strcmp((const char*)*attr, "MaxHeight") == 0)
            dash->current_representation_->height_ =
                static_cast<uint16_t>(atoi((const char*)*(attr + 1)));
          else if (strcmp((const char*)*attr, "SamplingRate") == 0)
            dash->current_representation_->samplingRate_ =
                static_cast<uint32_t>(atoi((const char*)*(attr + 1)));
          else if (strcmp((const char*)*attr, "Channels") == 0)
            dash->current_representation_->channelCount_ =
                static_cast<uint8_t>(atoi((const char*)*(attr + 1)));
          else if (strcmp((const char*)*attr, "Index") == 0)
            dash->current_representation_->id = (const char*)*(attr + 1);
          else if (strcmp((const char*)*attr, "CodecPrivateData") == 0)
            codecPrivateData = (const char*)*(attr + 1);
          else if (strcmp((const char*)*attr, "NALUnitLengthField") == 0)
            dash->current_representation_->nalLengthSize_ =
                static_cast<uint8_t>(atoi((const char*)*(attr + 1)));
          attr += 2;
        }

        if (codecPrivateData)
        {
          if (dash->current_representation_->codecs_.compare(0, 3, "hev") == 0)
            dash->current_representation_->codec_private_data_ = annexb_to_hvcc(codecPrivateData);
          else
            dash->current_representation_->codec_private_data_ = annexb_to_avc(codecPrivateData);
        }

        if (dash->current_representation_->codecs_ == "aacl" &&
            dash->current_representation_->codec_private_data_.empty())
        {
          uint16_t esds(0x1010), sidx(4);
          switch (dash->current_representation_->samplingRate_)
          {
            case 96000:
              sidx = 0;
              break;
            case 88200:
              sidx = 1;
              break;
            case 64000:
              sidx = 2;
              break;
            case 48000:
              sidx = 3;
              break;
            case 44100:
              sidx = 4;
              break;
            case 32000:
              sidx = 5;
              break;
            default:;
          }
          esds |= (sidx << 7);

          dash->current_representation_->codec_private_data_.resize(2);
          dash->current_representation_->codec_private_data_[0] = esds >> 8;
          dash->current_representation_->codec_private_data_[1] = esds & 0xFF;
        }

        dash->current_representation_->segtpl_.media.replace(pos, 9, bw);
        dash->current_representation_->bandwidth_ = atoi(bw);
        dash->current_adaptationset_->representations_.push_back(dash->current_representation_);
      }
      else if (strcmp(el, "c") == 0)
      {
        //<c n = "0" d = "20000000" / >
        uint32_t push_duration(~0);
        uint32_t repeat_count(1);

        for (; *attr;)
        {
          if (*(const char*)*attr == 't')
          {
            uint64_t lt(atoll((const char*)*(attr + 1)));
            if (!dash->current_adaptationset_->segment_durations_.data.empty())
            {
              //Go back to the previous timestamp to calculate the real gap.
              dash->pts_helper_ -= dash->current_adaptationset_->segment_durations_.data.back();
              dash->current_adaptationset_->segment_durations_.data.back() =
                  static_cast<uint32_t>(lt - dash->pts_helper_);
            }
            else
              dash->current_adaptationset_->startPTS_ = lt;
            dash->pts_helper_ = lt;
            if (!~push_duration)
              push_duration = 0;
          }
          else if (*(const char*)*attr == 'd')
            push_duration = atoi((const char*)*(attr + 1));
          else if (*(const char*)*attr == 'r')
            repeat_count = atoi((const char*)*(attr + 1));
          attr += 2;
        }
        if (~push_duration)
        {
          while (repeat_count--)
          {
            dash->current_adaptationset_->segment_durations_.data.push_back(push_duration);
            dash->pts_helper_ += push_duration;
          }
        }
      }
    }
    else if (strcmp(el, "StreamIndex") == 0)
    {
      //<StreamIndex Type = "video" TimeScale = "10000000" Name = "video" Chunks = "3673" QualityLevels = "6" Url = "QualityLevels({bitrate})/Fragments(video={start time})" MaxWidth = "960" MaxHeight = "540" DisplayWidth = "960" DisplayHeight = "540">
      dash->current_adaptationset_ = new SmoothTree::AdaptationSet();
      dash->current_period_->adaptationSets_.push_back(dash->current_adaptationset_);

      for (; *attr;)
      {
        if (strcmp((const char*)*attr, "Type") == 0)
          dash->current_adaptationset_->type_ =
              stricmp((const char*)*(attr + 1), "video") == 0
                  ? SmoothTree::VIDEO
                  : stricmp((const char*)*(attr + 1), "audio") == 0
                        ? SmoothTree::AUDIO
                        : stricmp((const char*)*(attr + 1), "text") == 0 ? SmoothTree::SUBTITLE
                                                                         : SmoothTree::NOTYPE;
        else if (strcmp((const char*)*attr, "Language") == 0)
          dash->current_adaptationset_->language_ = (const char*)*(attr + 1);
        else if (strcmp((const char*)*attr, "TimeScale") == 0)
          dash->current_adaptationset_->timescale_ = atoi((const char*)*(attr + 1));
        else if (strcmp((const char*)*attr, "Chunks") == 0)
          dash->current_adaptationset_->segment_durations_.data.reserve(
              atoi((const char*)*(attr + 1)));
        else if (strcmp((const char*)*attr, "Url") == 0)
          dash->current_adaptationset_->base_url_ = dash->base_url_ + (const char*)*(attr + 1);
        attr += 2;
      }
      dash->segcount_ = 0;
      if (!dash->current_adaptationset_->timescale_)
        dash->current_adaptationset_->timescale_ =
            10000000; //Defalt Smmoothstream frequency (10Khz)


      dash->currentNode_ |= SmoothTree::SSMNODE_STREAMINDEX;
    }
    else if (strcmp(el, "Protection") == 0)
    {
      dash->currentNode_ |= SmoothTree::SSMNODE_PROTECTION;
      dash->current_period_->encryptionState_ = SmoothTree::ENCRYTIONSTATE_SUPPORTED;
      dash->current_period_->need_secure_decoder_ = true;
    }
  }
  else if (strcmp(el, "SmoothStreamingMedia") == 0)
  {
    uint64_t timeScale = 0, duration = 0;
    dash->overallSeconds_ = 0;
    for (; *attr;)
    {
      if (strcmp((const char*)*attr, "TimeScale") == 0)
        timeScale = atoll((const char*)*(attr + 1));
      else if (strcmp((const char*)*attr, "Duration") == 0)
        duration = atoll((const char*)*(attr + 1));
      else if (strcmp((const char*)*attr, "IsLive") == 0)
      {
        dash->has_timeshift_buffer_ = strcmp((const char*)*(attr + 1), "TRUE") == 0;
        if (dash->has_timeshift_buffer_)
        {
          dash->stream_start_ = time(0);
          dash->available_time_ = dash->stream_start_;
        }
      }
      attr += 2;
    }
    if (!timeScale)
      timeScale = 10000000; //Defalt Smmoothstream frequency (10Khz)

    dash->overallSeconds_ = duration / timeScale;
    dash->currentNode_ |= SmoothTree::SSMNODE_SSM;
    dash->minPresentationOffset = ~0ULL;
    dash->base_time_ = ~0ULL;
  }
}

/*----------------------------------------------------------------------
|   expat text
+---------------------------------------------------------------------*/
static void XMLCALL text(void* data, const char* s, int len)
{
  SmoothTree* dash(reinterpret_cast<SmoothTree*>(data));

  if (dash->currentNode_ & SmoothTree::SSMNODE_PROTECTIONTEXT)
    dash->strXMLText_ += std::string(s, len);
}

/*----------------------------------------------------------------------
|   expat end
+---------------------------------------------------------------------*/
static void XMLCALL end(void* data, const char* el)
{
  SmoothTree* dash(reinterpret_cast<SmoothTree*>(data));

  if (dash->currentNode_ & SmoothTree::SSMNODE_SSM)
  {
    if (dash->currentNode_ & SmoothTree::SSMNODE_PROTECTION)
    {
      if (dash->currentNode_ & SmoothTree::SSMNODE_PROTECTIONHEADER)
      {
        if (strcmp(el, "ProtectionHeader") == 0)
          dash->currentNode_ &= ~SmoothTree::SSMNODE_PROTECTIONHEADER;
      }
      else if (strcmp(el, "Protection") == 0)
      {
        dash->currentNode_ &=
            ~(SmoothTree::SSMNODE_PROTECTION | SmoothTree::SSMNODE_PROTECTIONTEXT);
        PRProtectionParser parser(dash->strXMLText_);
        dash->current_defaultKID_ = parser.getKID();
        dash->license_url_ = parser.getLicenseURL();
        dash->current_pssh_ = parser.getPSSH();
        dash->strXMLText_.clear();
      }
    }
    else if (dash->currentNode_ & SmoothTree::SSMNODE_STREAMINDEX)
    {
      if (strcmp(el, "StreamIndex") == 0)
      {
        if (dash->current_adaptationset_->representations_.empty() ||
            dash->current_adaptationset_->segment_durations_.data.empty())
          dash->current_period_->adaptationSets_.pop_back();
        else
        {
          if (dash->current_adaptationset_->startPTS_ < dash->base_time_)
            dash->base_time_ = dash->current_adaptationset_->startPTS_;
        }
        dash->currentNode_ &= ~SmoothTree::SSMNODE_STREAMINDEX;
      }
    }
    else if (strcmp(el, "SmoothStreamingMedia") == 0)
      dash->currentNode_ &= ~SmoothTree::SSMNODE_SSM;
  }
}


/*----------------------------------------------------------------------
|   SmoothTree
+---------------------------------------------------------------------*/

bool SmoothTree::open(const std::string& url, const std::string& manifestUpdateParam)
{
  return open(url, manifestUpdateParam, std::map<std::string, std::string>());
}

bool SmoothTree::open(const std::string& url, const std::string& manifestUpdateParam, std::map<std::string, std::string> additionalHeaders)
{
  parser_ = XML_ParserCreate(NULL);
  if (!parser_)
    return false;
  XML_SetUserData(parser_, (void*)this);
  XML_SetElementHandler(parser_, start, end);
  XML_SetCharacterDataHandler(parser_, text);
  currentNode_ = 0;
  strXMLText_.clear();

  PrepareManifestUrl(url, manifestUpdateParam);
  additionalHeaders.insert(manifest_headers_.begin(), manifest_headers_.end());
  bool ret = download(manifest_url_.c_str(), additionalHeaders);

  XML_ParserFree(parser_);
  parser_ = 0;

  if (!ret)
    return false;

  uint8_t psshset(0);

  if (!current_defaultKID_.empty())
    psshset = static_cast<uint8_t>(insert_psshset(STREAM_TYPE_COUNT));

  for (std::vector<AdaptationSet*>::iterator ba(current_period_->adaptationSets_.begin()),
       ea(current_period_->adaptationSets_.end());
       ba != ea; ++ba)
  {
    for (std::vector<SmoothTree::Representation*>::iterator b((*ba)->representations_.begin()),
         e((*ba)->representations_.end());
         b != e; ++b)
    {
      (*b)->segments_.data.resize((*ba)->segment_durations_.data.size());
      std::vector<uint32_t>::iterator bsd((*ba)->segment_durations_.data.begin());
      uint64_t cummulated((*ba)->startPTS_ - base_time_), index(1);

      for (std::vector<SmoothTree::Segment>::iterator bs((*b)->segments_.data.begin()),
           es((*b)->segments_.data.end());
           bs != es; ++bsd, ++bs, ++index)
      {
        bs->startPTS_ = cummulated;
        bs->range_begin_ = cummulated + base_time_;
        bs->range_end_ = index;
        cummulated += *bsd;
      }
      (*b)->pssh_set_ = psshset;
    }
  }

  SortTree();

  return true;
}

bool SmoothTree::write_data(void* buffer, size_t buffer_size, void* opaque)
{
  bool done(false);
  XML_Status retval = XML_Parse(parser_, (const char*)buffer, buffer_size, done);

  if (retval == XML_STATUS_ERROR)
    return false;
  return true;
}
