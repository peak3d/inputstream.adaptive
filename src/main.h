
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

#include <vector>

#include <kodi/addon-instance/Inputstream.h>

#include "common/AdaptiveTree.h"
#include "common/AdaptiveStream.h"
#include <float.h>

#include "Ap4.h"
#include "SSD_dll.h"

class SampleReader;
struct DefaultRepresentationChooser;

namespace XBMCFILE
{
  /* indicate that caller can handle truncated reads, where function returns before entire buffer has been filled */
  static const unsigned int READ_TRUNCATED = 0x01;

  /* indicate that that caller support read in the minimum defined chunk size, this disables internal cache then */
  static const unsigned int READ_CHUNKED   = 0x02;

  /* use cache to access this file */
  static const unsigned int READ_CACHED    = 0x04;

  /* open without caching. regardless to file type. */
  static const unsigned int READ_NO_CACHE  = 0x08;

  /* calcuate bitrate for file while reading */
  static const unsigned int READ_BITRATE   = 0x10;
}

/*******************************************************
Kodi Streams implementation
********************************************************/

class KodiAdaptiveTree : public adaptive::AdaptiveTree
{
protected:
  virtual bool download(const char* url);
};

class KodiAdaptiveStream : public adaptive::AdaptiveStream
{
public:
  KodiAdaptiveStream(adaptive::AdaptiveTree& tree,
                     adaptive::AdaptiveTree::AdaptationSet* adp,
                     const std::map<std::string, std::string>& media_headers,
                     DefaultRepresentationChooser* chooser,
                     bool play_timeshift_buffer,
                     size_t repId)
    : adaptive::AdaptiveStream(tree, adp, media_headers, play_timeshift_buffer, repId),
      chooser_(chooser){};

protected:
  virtual bool download(const char* url, const std::map<std::string, std::string> &mediaHeaders) override;
  virtual bool parseIndexRange() override;

private:
  DefaultRepresentationChooser* chooser_ = nullptr;
};

enum MANIFEST_TYPE
{
  MANIFEST_TYPE_UNKNOWN,
  MANIFEST_TYPE_MPD,
  MANIFEST_TYPE_ISM,
  MANIFEST_TYPE_HLS
};

class Session: public adaptive::AdaptiveStreamObserver
{
public:
  Session(MANIFEST_TYPE manifestType,
          const char* strURL,
          const char* strUpdateParam,
          const char* strLicType,
          const char* strLicKey,
          const char* strLicData,
          const char* strCert,
          const char* strMediaRenewalUrl,
          const uint32_t intMediaRenewalTime,
          const std::map<std::string, std::string>& manifestHeaders,
          const std::map<std::string, std::string>& mediaHeaders,
          const char* profile_path,
          const char* ov_audio,
          bool play_timeshift_buffer,
          bool force_secure_decoder);
  virtual ~Session();
  bool Initialize(const std::uint8_t config, uint32_t max_user_bandwidth);
  bool InitializeDRM();
  bool InitializePeriod();
  SampleReader *GetNextSample();

  struct STREAM
  {
    STREAM(adaptive::AdaptiveTree& t,
           adaptive::AdaptiveTree::AdaptationSet* adp,
           const std::map<std::string, std::string>& media_headers,
           DefaultRepresentationChooser* chooser,
           bool play_timeshift_buffer,
           size_t repId)
      : enabled(false),
        encrypted(false),
        mainId_(0),
        current_segment_(0),
        stream_(t, adp, media_headers, chooser, play_timeshift_buffer, repId),
        input_(0),
        input_file_(0),
        reader_(0),
        segmentChanged(false),
        valid(true)
    {
      memset(&info_, 0, sizeof(info_));
    };
    ~STREAM()
    {
      disable();
      free((void*)info_.m_ExtraData), info_.m_ExtraData = nullptr;
#if INPUTSTREAM_VERSION_LEVEL > 0
      delete info_.m_masteringMetadata, info_.m_masteringMetadata = nullptr;
      delete info_.m_contentLightMetadata, info_.m_contentLightMetadata = nullptr;
#endif
    };
    void disable();

    bool enabled, encrypted;
    uint16_t mainId_;
    uint32_t current_segment_;
    KodiAdaptiveStream stream_;
    AP4_ByteStream *input_;
    AP4_File *input_file_;
    INPUTSTREAM_INFO info_;
    SampleReader *reader_;
    bool segmentChanged;
    bool valid;
  };

  void UpdateStream(STREAM &stream);
  AP4_Movie* PrepareStream(STREAM* stream, bool& needRefetch);

  STREAM* GetStream(unsigned int sid)const { return sid - 1 < streams_.size() ? streams_[sid - 1] : 0; };
  void EnableStream(STREAM* stream, bool enable);
  unsigned int GetStreamCount() const { return streams_.size(); };
  const char *GetCDMSession(int nSet) { return cdm_sessions_[nSet].cdm_session_str_; };;
  uint8_t GetMediaTypeMask() const { return media_type_mask_; };
  AP4_CencSingleSampleDecrypter * GetSingleSampleDecryptor(unsigned int nIndex)const{ return cdm_sessions_[nIndex].single_sample_decryptor_; };
  SSD::SSD_DECRYPTER *GetDecrypter() { return decrypter_; };
  AP4_CencSingleSampleDecrypter *GetSingleSampleDecrypter(std::string sessionId);
  const SSD::SSD_DECRYPTER::SSD_CAPS &GetDecrypterCaps(unsigned int nIndex) const{ return cdm_sessions_[nIndex].decrypter_caps_; };
  uint64_t GetTotalTimeMs()const { return adaptiveTree_->overallSeconds_ * 1000; };
  uint64_t GetElapsedTimeMs()const { return elapsed_time_ / 1000; };
  uint64_t PTSToElapsed(uint64_t pts);
  uint64_t GetTimeshiftBufferStart();
  bool CheckChange(bool bSet = false){ bool ret = changed_; changed_ = bSet; return ret; };
  void SetVideoResolution(unsigned int w, unsigned int h);
  bool SeekTime(double seekTime, unsigned int streamId = 0, bool preceeding=true);
  bool IsLive() const { return adaptiveTree_->has_timeshift_buffer_; };
  MANIFEST_TYPE GetManifestType() const { return manifest_type_; };
  const AP4_UI08 *GetDefaultKeyId(const uint16_t index) const;
  uint32_t GetIncludedStreamMask() const;
  CRYPTO_INFO::CRYPTO_KEY_SYSTEM GetCryptoKeySystem() const;

  int GetChapter() const;
  int GetChapterCount() const;
  const char* GetChapterName(int ch) const;
  int64_t GetChapterPos(int ch) const;
  bool SeekChapter(int ch);
  uint64_t GetChapterStartTime() { return chapter_start_time_; };
  double GetChapterSeekTime() { return chapter_seek_time_; };
  void ResetChapterSeekTime() { chapter_seek_time_ = 0; };

  //Observer Section
  void OnSegmentChanged(adaptive::AdaptiveStream *stream) override;
  void OnStreamChange(adaptive::AdaptiveStream *stream) override;

protected:
  void CheckFragmentDuration(STREAM &stream);
  void GetSupportedDecrypterURN(std::string &key_system);
  void DisposeSampleDecrypter();
  void DisposeDecrypter();

private:
  MANIFEST_TYPE manifest_type_;
  std::string mpdFileURL_, mpdUpdateParam_;
  std::string license_key_, license_type_, license_data_;
  std::map<std::string, std::string> media_headers_;
  AP4_DataBuffer server_certificate_;
  std::string profile_path_;
  std::string ov_audio_;
  void * decrypterModule_;
  SSD::SSD_DECRYPTER *decrypter_;

  struct CDMSESSION
  {
    SSD::SSD_DECRYPTER::SSD_CAPS decrypter_caps_;
    AP4_CencSingleSampleDecrypter *single_sample_decryptor_;
    const char *cdm_session_str_;
    bool shared_single_sample_decryptor_;
  };
  std::vector<CDMSESSION> cdm_sessions_;

  adaptive::AdaptiveTree* adaptiveTree_;
  DefaultRepresentationChooser* representationChooser_;

  std::vector<STREAM*> streams_;
  STREAM* timing_stream_;

  uint32_t fixed_bandwidth_;
  bool changed_;
  int manual_streams_;
  uint64_t elapsed_time_, chapter_start_time_; // In DVD_TIME_BASE
  double chapter_seek_time_; // In seconds
  uint8_t media_type_mask_;
  uint8_t drmConfig_;
  bool play_timeshift_buffer_;
  bool force_secure_decoder_;
};
