/*
*      Copyright (C) 2017 peak3d
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

#include "HLSTree.h"

#include "../Iaes_decrypter.h"
#include "../helpers.h"
#include "../log.h"

#include <algorithm>
#include <map>
#include <string.h>
#include <thread>

using namespace adaptive;

static void parseLine(const std::string& line,
                      size_t offset,
                      std::map<std::string, std::string>& map)
{
  size_t value, end;
  map.clear();
  while (offset < line.size() && (value = line.find('=', offset)) != std::string::npos)
  {
    while (offset < line.size() && line[offset] == ' ')
      ++offset;
    end = value;
    uint8_t inValue(0);
    while (++end < line.size() && ((inValue & 1) || line[end] != ','))
      if (line[end] == '\"')
        ++inValue;
    map[line.substr(offset, value - offset)] =
        line.substr(value + (inValue ? 2 : 1), end - value - (inValue ? 3 : 1));
    offset = end + 1;
  }
}

static void parseResolution(std::uint16_t& width, std::uint16_t& height, const std::string& val)
{
  std::string::size_type pos(val.find('x'));
  if (pos != std::string::npos)
  {
    width = atoi(val.c_str());
    height = atoi(val.c_str() + pos + 1);
  }
}

static std::string getVideoCodec(const std::string& codecs)
{
  if (codecs.empty())
    return "h264";
  else if (codecs.find("avc1.") != std::string::npos)
    return "h264";
  else if (codecs.find("hvc1.") != std::string::npos)
    return "hvc1";
  else if (codecs.find("hev1.") != std::string::npos)
    return "hev1";
  else if (codecs.find("dvh1.") != std::string::npos)
    return "dvh1";
  else if (codecs.find("dvhe.") != std::string::npos)
    return "dvhe";
  else
    return "";
}

static std::string getAudioCodec(const std::string& codecs)
{
  if (codecs.find("ec-3") != std::string::npos)
    return "ec-3";
  else if (codecs.find("ac-3") != std::string::npos)
    return "ac-3";
  else
    return "aac";
}

HLSTree::~HLSTree()
{
  delete m_decrypter;
}

int HLSTree::processEncryption(std::string baseUrl, std::map<std::string, std::string>& map)
{
  // NO ENCRYPTION
  if (map["METHOD"] == "NONE")
  {
    current_pssh_.clear();

    return ENCRYPTIONTYPE_CLEAR;
  }

  // AES-128
  if (map["METHOD"] == "AES-128" && !map["URI"].empty())
  {
    current_pssh_ = map["URI"];
    if (current_pssh_[0] != '/' && current_pssh_.find("://") == std::string::npos)
      current_pssh_ = baseUrl + current_pssh_;

    current_iv_ = m_decrypter->convertIV(map["IV"]);

    return ENCRYPTIONTYPE_AES128;
  }

  // WIDEVINE
  if (map["KEYFORMAT"] == "urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed" && !map["URI"].empty())
  {
    if (!map["KEYID"].empty())
    {
      std::string keyid = map["KEYID"].substr(2);
      const char* defaultKID = keyid.c_str();
      current_defaultKID_.resize(16);
      for (unsigned int i(0); i < 16; ++i)
      {
        current_defaultKID_[i] = HexNibble(*defaultKID) << 4;
        ++defaultKID;
        current_defaultKID_[i] |= HexNibble(*defaultKID);
        ++defaultKID;
      }
    }

    current_pssh_ = map["URI"].substr(23);
    // Try to get KID from pssh, we assume len+'pssh'+version(0)+systemid+lenkid+kid
    if (current_defaultKID_.empty() && current_pssh_.size() == 68)
    {
      unsigned int bufLen = 52;
      uint8_t buf[52];
      b64_decode(current_pssh_.c_str(), current_pssh_.size(), buf, bufLen);
      if (bufLen == 50)
        current_defaultKID_ = std::string(reinterpret_cast<const char*>(&buf[34]), 16);
    }

    return ENCRYPTIONTYPE_WIDEVINE;
  }

  // KNOWN UNSUPPORTED
  if (map["METHOD"] == "SAMPLE-AES")
  {
    Log(LOGLEVEL_ERROR, "Unsupported encryption method: %s", map["METHOD"].c_str());
    return ENCRYPTIONTYPE_INVALID;
  }

  return ENCRYPTIONTYPE_UNKNOWN;
}

bool HLSTree::open(const std::string& url, const std::string& manifestUpdateParam)
{
  return open(url, manifestUpdateParam, std::map<std::string, std::string>());
}

bool HLSTree::open(const std::string& url, const std::string& manifestUpdateParam, std::map<std::string, std::string> additionalHeaders)
{
  PrepareManifestUrl(url, manifestUpdateParam);
  additionalHeaders.insert(manifest_headers_.begin(), manifest_headers_.end());
  if (download(manifest_url_.c_str(), additionalHeaders, &manifest_stream))
    return processManifest(manifest_stream);
  return false;
}

bool HLSTree::processManifest(std::stringstream& stream)
{
#if FILEDEBUG
  FILE* f = fopen("inputstream_adaptive_master.m3u8", "w");
  fwrite(m_stream.str().data(), 1, m_stream.str().size(), f);
  fclose(f);
#endif

  std::string line;
  bool startCodeFound = false;

  current_adaptationset_ = nullptr;
  current_representation_ = nullptr;

  periods_.push_back(new Period());
  current_period_ = periods_.back();
  current_period_->timescale_ = 1000000;

  std::map<std::string, std::string> map;

  while (std::getline(stream, line))
  {
    if (!startCodeFound)
    {
      if (line.compare(0, 7, "#EXTM3U") == 0)
        startCodeFound = true;
      continue;
    }

    std::string::size_type sz(line.size());
    while (sz && (line[sz - 1] == '\r' || line[sz - 1] == '\n' || line[sz - 1] == ' '))
      --sz;
    line.resize(sz);

    if (line.compare(0, 13, "#EXT-X-MEDIA:") == 0)
    {
      //#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="bipbop_audio",LANGUAGE="eng",NAME="BipBop Audio 2",AUTOSELECT=NO,DEFAULT=NO,URI="alternate_audio_aac_sinewave/prog_index.m3u8"
      parseLine(line, 13, map);

      StreamType type;
      if (map["TYPE"] == "AUDIO")
        type = AUDIO;
      else if (map["TYPE"] == "SUBTITLES")
        type = SUBTITLE;
      else
        continue;

      EXTGROUP& group = m_extGroups[map["GROUP-ID"]];

      AdaptationSet* adp = new AdaptationSet();
      Representation* rep = new Representation();
      adp->representations_.push_back(rep);
      group.m_sets.push_back(adp);

      adp->type_ = type;
      adp->language_ = map["LANGUAGE"];
      adp->timescale_ = 1000000;
      adp->name_ = map["NAME"];
      adp->default_ = map["DEFAULT"] == "YES";
      adp->forced_ = map["FORCED"] == "YES";

      rep->codecs_ = group.m_codec;
      rep->timescale_ = 1000000;
      rep->containerType_ = CONTAINERTYPE_NOTYPE;

      std::map<std::string, std::string>::iterator res;
      if ((res = map.find("URI")) != map.end())
      {
        rep->source_url_ = BuildDownloadUrl(res->second);

        // default to WebVTT
        if (type == SUBTITLE)
          rep->codecs_ = "wvtt";
      }
      else
      {
        rep->flags_ = Representation::INCLUDEDSTREAM;
        current_period_->included_types_ |= 1U << type;
      }

      if ((res = map.find("CHANNELS")) != map.end())
        rep->channelCount_ = atoi(res->second.c_str());
    }
    else if (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0)
    {
      // TODO: If CODECS value is not present, get StreamReps from stream program section
      //#EXT-X-STREAM-INF:BANDWIDTH=263851,CODECS="mp4a.40.2, avc1.4d400d",RESOLUTION=416x234,AUDIO="bipbop_audio",SUBTITLES="subs"
      parseLine(line, 18, map);

      current_representation_ = nullptr;

      if (map.find("BANDWIDTH") == map.end())
        continue;

      if (!current_adaptationset_)
      {
        current_adaptationset_ = new AdaptationSet();
        current_adaptationset_->type_ = VIDEO;
        current_adaptationset_->timescale_ = 1000000;
        current_period_->adaptationSets_.push_back(current_adaptationset_);
      }
      current_representation_ = new Representation();
      current_adaptationset_->representations_.push_back(current_representation_);
      current_representation_->timescale_ = 1000000;
      current_representation_->codecs_ = getVideoCodec(map["CODECS"]);
      current_representation_->bandwidth_ = atoi(map["BANDWIDTH"].c_str());
      current_representation_->containerType_ = CONTAINERTYPE_NOTYPE;

      if (map.find("RESOLUTION") != map.end())
        parseResolution(current_representation_->width_, current_representation_->height_,
                        map["RESOLUTION"]);

      if (map.find("AUDIO") != map.end())
        m_extGroups[map["AUDIO"]].setCodec(getAudioCodec(map["CODECS"]));
      else
      {
        // We assume audio is included
        current_period_->included_types_ |= 1U << AUDIO;
        m_audioCodec = getAudioCodec(map["CODECS"]);
      }
      if (map.find("FRAME-RATE") != map.end())
      {
        current_representation_->fpsRate_ = static_cast<int>(std::stof(map["FRAME-RATE"]) * 1000);
        current_representation_->fpsScale_ = 1000;
      }
    }
    else if (line.compare(0, 8, "#EXTINF:") == 0)
    {
      //Uh, this is not a multi - bitrate playlist
      current_adaptationset_ = new AdaptationSet();
      current_adaptationset_->type_ = VIDEO;
      current_adaptationset_->timescale_ = 1000000;
      current_period_->adaptationSets_.push_back(current_adaptationset_);

      current_representation_ = new Representation();
      current_representation_->timescale_ = 1000000;
      current_representation_->bandwidth_ = 0;
      current_representation_->codecs_ = getVideoCodec("");
      current_representation_->containerType_ = CONTAINERTYPE_NOTYPE;
      current_representation_->source_url_ = manifest_url_;
      current_adaptationset_->representations_.push_back(current_representation_);

      // We assume audio is included
      current_period_->included_types_ |= 1U << AUDIO;
      m_audioCodec = getAudioCodec("");
      break;
    }
    else if (!line.empty() && line.compare(0, 1, "#") != 0 && current_representation_)
    {
      current_representation_->source_url_ = BuildDownloadUrl(line);

      //Ignore duplicate reps
      for (auto const* rep : current_adaptationset_->representations_)
        if (rep != current_representation_ &&
            rep->source_url_ == current_representation_->source_url_)
        {
          delete current_representation_;
          current_representation_ = nullptr;
          current_adaptationset_->representations_.pop_back();
          break;
        }
    }
    else if (line.compare(0, 19, "#EXT-X-SESSION-KEY:") == 0)
    {
      parseLine(line, 19, map);
      uint32_t encryption_type;
      switch (encryption_type = processEncryption(base_url_, map))
      {
        case ENCRYPTIONTYPE_INVALID:
          return false;
        case ENCRYPTIONTYPE_AES128:
        case ENCRYPTIONTYPE_WIDEVINE:
          // #EXT-X-SESSION-KEY is meant for preparing DRM without
          // loading sub-playlist. As long our workfow is serial, we
          // don't profite and therefore do not any action.
          break;
        default:;
      }
    }
  }

  if (current_period_)
  {
    // We may need to create the Default / Dummy audio representation
    if (!m_audioCodec.empty())
    {
      current_adaptationset_ = new AdaptationSet();
      current_adaptationset_->type_ = AUDIO;
      current_adaptationset_->timescale_ = 1000000;
      current_period_->adaptationSets_.push_back(current_adaptationset_);

      current_representation_ = new Representation();
      current_representation_->timescale_ = 1000000;
      current_representation_->codecs_ = m_audioCodec;
      current_representation_->flags_ = Representation::INCLUDEDSTREAM;
      current_adaptationset_->representations_.push_back(current_representation_);
    }

    //Register external adaptationsets
    for (const auto& group : m_extGroups)
      for (auto* adp : group.second.m_sets)
        current_period_->adaptationSets_.push_back(adp);
    m_extGroups.clear();

    SortTree();
  }
  // Set Live as default
  has_timeshift_buffer_ = true;
  update_parameter_ = "full";
  return true;
}

HLSTree::PREPARE_RESULT HLSTree::prepareRepresentation(Period* period,
                                                       AdaptationSet* adp,
                                                       Representation* rep,
                                                       bool update)
{
  if (!rep->source_url_.empty())
  {
    SPINCACHE<Segment> newSegments;
    unsigned int newStartNumber;
    Segment newInitialization;
    uint32_t segmentId(rep->getCurrentSegmentNumber());
    std::stringstream stream;
    uint32_t adp_pos =
        std::find(period->adaptationSets_.begin(), period->adaptationSets_.end(), adp) -
        period->adaptationSets_.begin();
    uint32_t rep_pos = std::find(adp->representations_.begin(), adp->representations_.end(), rep) -
                       adp->representations_.begin();
    uint32_t discont_count = 0;
    bool cp_lost(false);
    Representation* entry_rep = rep;
    PREPARE_RESULT retVal = PREPARE_RESULT_OK;

    if (rep->flags_ & Representation::DOWNLOADED)
      ;
    else if (download(rep->source_url_.c_str(), manifest_headers_, &stream, false))
    {
#if FILEDEBUG
      FILE* f = fopen("inputstream_adaptive_sub.m3u8", "w");
      fwrite(m_stream.str().data(), 1, m_stream.str().size(), f);
      fclose(f);
#endif
      bool byteRange(false);
      bool segmentInitialization(false);
      bool hasMap(false);
      std::string line;
      std::string base_url;
      std::string map_url;

      std::map<std::string, std::string> map;
      bool startCodeFound(false);
      Segment segment;
      uint64_t pts(0);
      newStartNumber = 0;

      uint32_t currentEncryptionType = ENCRYPTIONTYPE_CLEAR;

      segment.range_begin_ = ~0ULL;
      segment.range_end_ = 0;
      segment.startPTS_ = ~0ULL;
      segment.pssh_set_ = 0;

      std::string::size_type paramPos = effective_url_.find('?');
      base_url =
          (paramPos == std::string::npos) ? effective_url_ : effective_url_.substr(0, paramPos);

      paramPos = base_url.rfind('/');
      if (paramPos != std::string::npos)
        base_url = base_url.substr(0, paramPos + 1);

      while (std::getline(stream, line))
      {
        if (!startCodeFound)
        {
          if (line.compare(0, 7, "#EXTM3U") == 0)
            startCodeFound = true;
          continue;
        }

        std::string::size_type sz(line.size());
        while (sz && (line[sz - 1] == '\r' || line[sz - 1] == '\n' || line[sz - 1] == ' '))
          --sz;
        line.resize(sz);

        if (line.compare(0, 8, "#EXTINF:") == 0)
        {
          segment.startPTS_ = pts;
          pts += static_cast<uint64_t>(atof(line.c_str() + 8) * rep->timescale_);
        }
        else if (line.compare(0, 17, "#EXT-X-BYTERANGE:") == 0)
        {
          std::string::size_type bs = line.rfind('@');
          if (bs != std::string::npos)
          {
            segment.range_begin_ = atoll(line.c_str() + (bs + 1));
            segment.range_end_ = segment.range_begin_ + atoll(line.c_str() + 17) - 1;
          }
          byteRange = true;
        }
        else if (!line.empty() && line.compare(0, 1, "#") != 0 && ~segment.startPTS_)
        {
          if (rep->containerType_ == CONTAINERTYPE_NOTYPE)
          {
            std::string::size_type paramPos = line.rfind('?');
            std::string::size_type ext = line.rfind('.', paramPos);
            if (ext != std::string::npos)
            {
              if (strncmp(line.c_str() + ext, ".ts", 3) == 0)
                rep->containerType_ = CONTAINERTYPE_TS;
              else if (strncmp(line.c_str() + ext, ".aac", 4) == 0)
                rep->containerType_ = CONTAINERTYPE_ADTS;
              else if (strncmp(line.c_str() + ext, ".mp4", 4) == 0)
                rep->containerType_ = CONTAINERTYPE_MP4;
              else if (strncmp(line.c_str() + ext, ".vtt", 4) == 0 ||
                       strncmp(line.c_str() + ext, ".webvtt", 7) == 0)
                rep->containerType_ = CONTAINERTYPE_TEXT;
              else
              {
                rep->containerType_ = CONTAINERTYPE_INVALID;
                continue;
              }
            }
            else
              //Fallback, assume .ts
              rep->containerType_ = CONTAINERTYPE_TS;
          }
          else if (rep->containerType_ == CONTAINERTYPE_INVALID)
            continue;

          if (!byteRange || rep->url_.empty())
          {
            std::string url;
            if (line[0] != '/' && line.find("://") == std::string::npos)
              url = base_url + line;
            else
              url = line;
            if (!byteRange)
            {
              segment.url = new char[url.size() + 1];
              memcpy((char*)segment.url, url.c_str(), url.size() + 1);
            }
            else
              rep->url_ = url;
          }
          if (currentEncryptionType == ENCRYPTIONTYPE_AES128)
          {
            if (segment.pssh_set_ == 0)
              segment.pssh_set_ = insert_psshset(NOTYPE, period, adp);
            else
              period->InsertPSSHSet(segment.pssh_set_);
          }
          newSegments.data.push_back(segment);
          segment.startPTS_ = ~0ULL;
        }
        else if (line.compare(0, 22, "#EXT-X-MEDIA-SEQUENCE:") == 0)
        {
          newStartNumber = atol(line.c_str() + 22);
        }
        else if (line.compare(0, 21, "#EXT-X-PLAYLIST-TYPE:") == 0)
        {
          if (strcmp(line.c_str() + 21, "VOD") == 0)
          {
            m_refreshPlayList = false;
            has_timeshift_buffer_ = false;
          }
        }
        else if (line.compare(0, 22, "#EXT-X-TARGETDURATION:") == 0)
        {
          uint32_t newInterval = atoi(line.c_str() + 22) * 1500;
          if (newInterval < updateInterval_)
            updateInterval_ = newInterval;
        }
        else if (line.compare(0, 30, "#EXT-X-DISCONTINUITY-SEQUENCE:") == 0)
        {
          m_discontSeq = atol(line.c_str() + 30);
          if (!~initial_sequence_)
            initial_sequence_ = m_discontSeq;
          m_hasDiscontSeq = true;
          // make sure first period has a sequence on initial prepare
          if (!update && m_discontSeq && !periods_.back()->sequence_)
            periods_[0]->sequence_ = m_discontSeq;

          auto bp = periods_.begin();
          while (bp != periods_.end())
          {
            if ((*bp)->sequence_ < m_discontSeq)
              if (*bp != current_period_)
              {
                delete *bp;
                *bp = nullptr;
                bp = periods_.erase(bp);
              }
              // we end up here after pausing for some time
              // remove from periods_ for now and reattach later
              else
              {
                cp_lost = true;
                bp = periods_.erase(bp);
              }
            else
              bp++;
          }
          period = periods_[0];
          adp = period->adaptationSets_[adp_pos];
          rep = adp->representations_[rep_pos];
        }
        else if (line.compare(0, 21, "#EXT-X-DISCONTINUITY") == 0)
        {
          period->sequence_ = m_discontSeq + discont_count;
          period->duration_ = newSegments.size() ? pts - newSegments[0]->startPTS_ : 0;
          if (!byteRange)
            rep->flags_ |= Representation::URLSEGMENTS;
          if (rep->containerType_ == CONTAINERTYPE_MP4 && byteRange && newSegments.size() &&
              newSegments.data[0].range_begin_ > 0)
          {
            rep->flags_ |= Representation::INITIALIZATION;
            rep->initialization_.range_begin_ = 0;
            rep->initialization_.range_end_ = newSegments.data[0].range_begin_ - 1;
            rep->initialization_.pssh_set_ = 0;
          }
          FreeSegments(period, rep);
          rep->segments_.swap(newSegments);
          rep->startNumber_ = newStartNumber;

          if (segmentInitialization)
          {
            std::swap(rep->initialization_, newInitialization);
            // EXT-X-MAP init url must persist to next period until overrided by new tag
            newInitialization.url = new char[map_url.size() + 1];
            memcpy((char*)newInitialization.url, map_url.c_str(), map_url.size() + 1);
          }
          if (periods_.size() == ++discont_count)
          {
            periods_.push_back(new Period());
            periods_.back()->CopyBasicData(current_period_);
            period = periods_.back();
          }
          else
            period = periods_[discont_count];

          newStartNumber += rep->segments_.data.size();
          adp = period->adaptationSets_[adp_pos];
          rep = adp->representations_[rep_pos];
          segment.range_begin_ = ~0ULL;
          segment.range_end_ = 0;
          segment.startPTS_ = ~0ULL;
          segment.pssh_set_ = 0;

          if (currentEncryptionType == ENCRYPTIONTYPE_WIDEVINE)
          {
            rep->pssh_set_ = insert_psshset(adp->type_, period, adp);
            period->encryptionState_ |= ENCRYTIONSTATE_SUPPORTED;
          }

          if (segmentInitialization && !map_url.empty())
          {
            rep->flags_ |= Representation::INITIALIZATION;
            rep->containerType_ = CONTAINERTYPE_MP4;
          }
        }
        else if (line.compare(0, 11, "#EXT-X-KEY:") == 0)
        {
          parseLine(line, 11, map);
          switch (processEncryption(base_url, map))
          {
            case ENCRYPTIONTYPE_INVALID:
              return PREPARE_RESULT_FAILURE;
            case ENCRYPTIONTYPE_AES128:
              currentEncryptionType = ENCRYPTIONTYPE_AES128;
              segment.pssh_set_ = 0;
              break;
            case ENCRYPTIONTYPE_WIDEVINE:
              currentEncryptionType = ENCRYPTIONTYPE_WIDEVINE;
              period->encryptionState_ |= ENCRYTIONSTATE_SUPPORTED;
              rep->pssh_set_ = insert_psshset(adp->type_, period, adp);
              retVal = period->psshSets_[rep->pssh_set_].use_count_ == 1 ||
                               retVal == PREPARE_RESULT_DRMCHANGED
                           ? PREPARE_RESULT_DRMCHANGED
                           : PREPARE_RESULT_DRMUNCHANGED;
              break;
            default:
              break;
          }
        }
        else if (line.compare(0, 14, "#EXT-X-ENDLIST") == 0)
        {
          m_refreshPlayList = false;
          has_timeshift_buffer_ = false;
        }
        else if (line.compare(0, 11, "#EXT-X-MAP:") == 0)
        {
          parseLine(line, 11, map);
          if (!map["URI"].empty())
          {
            if (!map["BYTERANGE"].empty())
              continue;
            // delete init url if persisted from previous period
            if (hasMap)
              delete[] newInitialization.url;
            segmentInitialization = true;
            std::string uri = map["URI"];
            if (uri[0] != '/' && uri.find("://") == std::string::npos)
              map_url = base_url + uri;
            else
              map_url = uri;
            newInitialization.url = new char[map_url.size() + 1];
            memcpy((char*)newInitialization.url, map_url.c_str(), map_url.size() + 1);
            newInitialization.range_begin_ = ~0ULL;
            newInitialization.startPTS_ = ~0ULL;
            newInitialization.pssh_set_ = 0;
            rep->flags_ |= Representation::INITIALIZATION;
            rep->containerType_ = CONTAINERTYPE_MP4;
            hasMap = true;
          }
        }
      }
      if (!byteRange)
        rep->flags_ |= Representation::URLSEGMENTS;

      // Insert Initialization Segment
      if (rep->containerType_ == CONTAINERTYPE_MP4 && byteRange &&
          newSegments.data[0].range_begin_ > 0)
      {
        rep->flags_ |= Representation::INITIALIZATION;
        rep->initialization_.range_begin_ = 0;
        rep->initialization_.range_end_ = newSegments.data[0].range_begin_ - 1;
        rep->initialization_.pssh_set_ = 0;
      }

      FreeSegments(period, rep);

      if (newSegments.data.empty())
      {
        FreeSegments(period, rep);
        rep->flags_ = 0;
        return PREPARE_RESULT_FAILURE;
      }

      rep->segments_.swap(newSegments);
      rep->startNumber_ = newStartNumber;

      if (segmentInitialization)
        std::swap(rep->initialization_, newInitialization);

      rep->duration_ = rep->segments_[0] ? (pts - rep->segments_[0]->startPTS_) : 0;

      period->sequence_ = m_discontSeq + discont_count;
      if (discont_count || m_hasDiscontSeq)
      {
        periods_[discont_count]->duration_ =
            (rep->duration_ * periods_[discont_count]->timescale_) / rep->timescale_;
        overallSeconds_ = 0;
        for (auto p : periods_)
        {
          overallSeconds_ += p->duration_ / p->timescale_;
          if (!has_timeshift_buffer_ && !m_refreshPlayList)
            p->adaptationSets_[adp_pos]->representations_[rep_pos]->flags_ |=
                Representation::DOWNLOADED;
        }
      }
      else
      {
        overallSeconds_ = rep->duration_ / rep->timescale_;
        if (!has_timeshift_buffer_ && !m_refreshPlayList)
          rep->flags_ |= Representation::DOWNLOADED;
      }
    }

    if (update)
    {
      rep = entry_rep;
      if (!segmentId || segmentId < rep->startNumber_ || !~segmentId)
        rep->current_segment_ = nullptr;
      else
      {
        if (segmentId >= rep->startNumber_ + rep->segments_.size())
          segmentId = rep->startNumber_ + rep->segments_.size() - 1;
        rep->current_segment_ = rep->get_segment(segmentId - rep->startNumber_);
      }
      if ((rep->flags_ & Representation::WAITFORSEGMENT) &&
          (rep->get_next_segment(rep->current_segment_) || current_period_ != periods_.back()))
        rep->flags_ &= ~Representation::WAITFORSEGMENT;
    }
    else
      StartUpdateThread();

    if (cp_lost)
      periods_.insert(periods_.begin(), current_period_);
    period = current_period_;
    adp = period->adaptationSets_[adp_pos];
    rep = adp->representations_[rep_pos];
    return retVal;
  }
  return PREPARE_RESULT_FAILURE;
};

bool HLSTree::write_data(void* buffer, size_t buffer_size, void* opaque)
{
  static_cast<std::stringstream*>(opaque)->write(static_cast<const char*>(buffer), buffer_size);
  return true;
}

void HLSTree::OnDataArrived(unsigned int segNum,
                            uint16_t psshSet,
                            uint8_t iv[16],
                            const uint8_t* src,
                            uint8_t* dst,
                            size_t dstOffset,
                            size_t dataSize)
{
  if (psshSet && current_period_->encryptionState_ != ENCRYTIONSTATE_SUPPORTED)
  {
    std::lock_guard<std::mutex> lck(treeMutex_);

    Period::PSSH& pssh(current_period_->psshSets_[psshSet]);
    //Encrypted media, decrypt it
    if (pssh.defaultKID_.empty())
    {
      //First look if we already have this URL resolved
      for (std::vector<Period::PSSH>::const_iterator b(current_period_->psshSets_.begin()),
           e(current_period_->psshSets_.end());
           b != e; ++b)
        if (b->pssh_ == pssh.pssh_ && !b->defaultKID_.empty())
        {
          pssh.defaultKID_ = b->defaultKID_;
          break;
        }
      if (pssh.defaultKID_.empty())
      {
      RETRY:
        std::stringstream stream;
        std::map<std::string, std::string> headers;
        std::vector<std::string> keyParts(split(m_decrypter->getLicenseKey(), '|'));
        std::string url = pssh.pssh_.c_str();

        if (keyParts.size() > 0 && !keyParts[0].empty())
        {
          if (url.find_first_of('?') == std::string::npos)
            url += "?";
          else
            url += "&";
          url += keyParts[0];
        }
        if (keyParts.size() > 1)
          parseheader(headers, keyParts[1].c_str());

        url = BuildDownloadUrl(url);
        if (download(url.c_str(), headers, &stream, false))
        {
          pssh.defaultKID_ = stream.str();
        }
        else if (pssh.defaultKID_ != "0")
        {
          pssh.defaultKID_ = "0";
          if (keyParts.size() >= 5 && !keyParts[4].empty() &&
              m_decrypter->RenewLicense(keyParts[4]))
            goto RETRY;
        }
      }
    }
    if (pssh.defaultKID_ == "0")
    {
      memset(dst + dstOffset, 0, dataSize);
      return;
    }
    else if (!dstOffset)
    {
      if (pssh.iv.empty())
        m_decrypter->ivFromSequence(iv, segNum);
      else
      {
        memset(iv, 0, 16);
        memcpy(iv, pssh.iv.data(), pssh.iv.size() < 16 ? pssh.iv.size() : 16);
      }
    }
    m_decrypter->decrypt(reinterpret_cast<const uint8_t*>(pssh.defaultKID_.data()), iv, src,
                         dst + dstOffset, dataSize);
    if (dataSize >= 16)
      memcpy(iv, src + (dataSize - 16), 16);
  }
  else
    AdaptiveTree::OnDataArrived(segNum, psshSet, iv, src, dst, dstOffset, dataSize);
}

//Called each time before we switch to a new segment
void HLSTree::RefreshSegments(Period* period,
                              AdaptationSet* adp,
                              Representation* rep,
                              StreamType type)
{
  if (m_refreshPlayList)
  {
    if (rep->flags_ & Representation::INCLUDEDSTREAM)
      return;
    RefreshUpdateThread();
    prepareRepresentation(period, adp, rep, true);
  }
}

//Called form update-thread
void HLSTree::RefreshLiveSegments()
{
  if (m_refreshPlayList)
  {
    std::vector<std::tuple<AdaptationSet*, Representation*>> refresh_list;
    for (std::vector<AdaptationSet*>::const_iterator ba(current_period_->adaptationSets_.begin()),
         ea(current_period_->adaptationSets_.end());
         ba != ea; ++ba)
      for (std::vector<Representation*>::iterator br((*ba)->representations_.begin()),
           er((*ba)->representations_.end());
           br != er; ++br)
        if ((*br)->flags_ & Representation::ENABLED)
          refresh_list.push_back(std::make_tuple(*ba, *br));
    for (auto t : refresh_list)
      prepareRepresentation(current_period_, std::get<0>(t), std::get<1>(t), true);
  }
}
