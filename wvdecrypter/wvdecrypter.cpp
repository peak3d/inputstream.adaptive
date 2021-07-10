/*
*      Copyright (C) 2016 liberty-developer
*      https://github.com/liberty-developer
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

#include "cdm/media/cdm/cdm_adapter.h"
#include "../src/helpers.h"
#include "../src/SSD_dll.h"
#include "../src/md5.h"
#include "jsmn.h"
#include "Ap4.h"

#include <stdarg.h>
#include <vector>
#include <list>
#include <algorithm>
#include <thread>

#ifndef WIDEVINECDMFILENAME
#error  "WIDEVINECDMFILENAME must be set"
#endif

#define LOCLICENSE 1

using namespace SSD;

SSD_HOST *host = 0;

static void Log(SSD_HOST::LOGLEVEL loglevel, const char *format, ...)
{
  char buffer[16384];
  va_list args;
  va_start(args, format);
  vsprintf(buffer, format, args);
  va_end(args);
  return host->Log(loglevel, buffer);
}

/*******************************************************
CDM
********************************************************/

/*----------------------------------------------------------------------
|   CdmDecryptedBlock implementation
+---------------------------------------------------------------------*/

class CdmDecryptedBlock : public cdm::DecryptedBlock {
public:
  CdmDecryptedBlock() :buffer_(0), timestamp_(0) {};
  virtual ~CdmDecryptedBlock() {};

  virtual void SetDecryptedBuffer(cdm::Buffer* buffer) override { buffer_ = buffer; };
  virtual cdm::Buffer* DecryptedBuffer() override { return buffer_; };

  virtual void SetTimestamp(int64_t timestamp) override { timestamp_ = timestamp; };
  virtual int64_t Timestamp() const override { return timestamp_; };
private:
  cdm::Buffer *buffer_;
  int64_t timestamp_;
};

/*----------------------------------------------------------------------
|   CdmDecryptedBlock implementation
+---------------------------------------------------------------------*/
class CdmBuffer : public cdm::Buffer {
public:
  CdmBuffer(AP4_DataBuffer *buffer) :buffer_(buffer) {};
  virtual ~CdmBuffer() {};

  virtual void Destroy() override {};

  virtual uint32_t Capacity() const override
  {
    return buffer_->GetBufferSize();
  };
  virtual uint8_t* Data() override
  {
    return (uint8_t*)buffer_->GetData();
  };
  virtual void SetSize(uint32_t size) override
  {
    buffer_->SetDataSize(size);
  };
  virtual uint32_t Size() const override
  {
    return buffer_->GetDataSize();
  };
private:
  AP4_DataBuffer *buffer_;
};

/*----------------------------------------------------------------------
|   CdmVideoDecoder implementation
+---------------------------------------------------------------------*/

class CdmFixedBuffer : public cdm::Buffer {
public:
  CdmFixedBuffer() : data_(nullptr), dataSize_(0), capacity_(0), buffer_(nullptr), instance_(nullptr) {};
  virtual ~CdmFixedBuffer() {};

  virtual void Destroy() override
  {
    host->ReleaseBuffer(instance_, buffer_);
    delete this;
  };

  virtual uint32_t Capacity() const override
  {
    return capacity_;
  };

  virtual uint8_t* Data() override
  {
    return data_;
  };

  virtual void SetSize(uint32_t size) override
  {
    dataSize_ = size;
  };

  virtual uint32_t Size() const override
  {
    return dataSize_;
  };

  void initialize(void *instance, uint8_t* data, size_t dataSize, void *buffer)
  {
    instance_ = instance;
    data_ = data;
    dataSize_ = 0;
    capacity_ = dataSize;
    buffer_ = buffer;
  }

  void *Buffer() const
  {
    return buffer_;
  };

private:
  uint8_t *data_;
  size_t dataSize_, capacity_;
  void *buffer_;
  void *instance_;
};



/*----------------------------------------------------------------------
|   WV_CencSingleSampleDecrypter
+---------------------------------------------------------------------*/
class WV_DRM;

class WV_CencSingleSampleDecrypter : public AP4_CencSingleSampleDecrypter
{
public:
  // methods
  WV_CencSingleSampleDecrypter(WV_DRM &drm, AP4_DataBuffer &pssh, const uint8_t *defaultKeyId, bool skipSessionMessage);
  virtual ~WV_CencSingleSampleDecrypter();

  void GetCapabilities(const uint8_t* key, uint32_t media, SSD_DECRYPTER::SSD_CAPS &caps);
  virtual const char *GetSessionId() override;
  void SetSessionActive();
  void CloseSessionId();
  AP4_DataBuffer GetChallengeData();

  void SetSession(const char* session, uint32_t session_size, const uint8_t *data, size_t data_size)
  {
    std::lock_guard<std::mutex> lock(renewal_lock_);

    session_ = std::string(session, session_size);
    challenge_.SetData(data, data_size);
    Log(SSD_HOST::LL_DEBUG, "%s: opened session with Id: %s", __func__, session_.c_str());
  }

  void AddSessionKey(const uint8_t *data, size_t data_size, uint32_t status);
  bool HasKeyId(const uint8_t *keyid);

  virtual AP4_Result SetFragmentInfo(AP4_UI32 pool_id, const AP4_UI08 *key, const AP4_UI08 nal_length_size,
    AP4_DataBuffer &annexb_sps_pps, AP4_UI32 flags)override;
  virtual AP4_UI32 AddPool() override;
  virtual void RemovePool(AP4_UI32 poolid) override;


  virtual AP4_Result DecryptSampleData(AP4_UI32 pool_id,
    AP4_DataBuffer& data_in,
    AP4_DataBuffer& data_out,

    // always 16 bytes
    const AP4_UI08* iv,

    // pass 0 for full decryption
    unsigned int    subsample_count,

    // array of <subsample_count> integers. NULL if subsample_count is 0
    const AP4_UI16* bytes_of_cleartext_data,

    // array of <subsample_count> integers. NULL if subsample_count is 0
    const AP4_UI32* bytes_of_encrypted_data) override;

  bool OpenVideoDecoder(const SSD_VIDEOINITDATA *initData);
  SSD_DECODE_RETVAL DecodeVideo(void* hostInstance, SSD_SAMPLE *sample, SSD_PICTURE *picture);
  void ResetVideo();

private:
  void CheckLicenseRenewal();
  bool SendSessionMessage();

  WV_DRM &drm_;
  std::string session_;
  AP4_DataBuffer pssh_, challenge_;
  uint8_t defaultKeyId_[16];
  struct WVSKEY
  {
    bool operator == (WVSKEY const &other) const { return keyid == other.keyid; };
    std::string keyid;
    cdm::KeyStatus status;
  };
  std::vector<WVSKEY> keys_;

  AP4_UI16 hdcp_version_;
  AP4_UI32 hdcp_limit_;
  AP4_UI32 resolution_limit_;

  unsigned int max_subsample_count_decrypt_, max_subsample_count_video_;
  cdm::SubsampleEntry *subsample_buffer_decrypt_, *subsample_buffer_video_;
  AP4_DataBuffer decrypt_in_, decrypt_out_;

  struct FINFO
  {
    const AP4_UI08 *key_;
    AP4_UI08 nal_length_size_;
    AP4_UI16 decrypter_flags_;
    AP4_DataBuffer annexb_sps_pps_;
  };
  std::vector<FINFO> fragment_pool_;

  uint32_t promise_id_;
  bool drained_;

  std::list<media::CdmVideoFrame> videoFrames_;
  std::mutex renewal_lock_;
};


class WV_DRM : public media::CdmAdapterClient
{
public:
  WV_DRM(const char* licenseURL, const AP4_DataBuffer &serverCert, const uint8_t config);
  virtual ~WV_DRM();

  virtual void OnCDMMessage(const char* session, uint32_t session_size, CDMADPMSG msg, const uint8_t *data, size_t data_size, uint32_t status) override;

  virtual void CDMLog(const char *msg) override
  {
    host->Log(SSD_HOST::LOGLEVEL::LL_DEBUG, msg);
  }

  virtual cdm::Buffer *AllocateBuffer(size_t sz) override
  {
    SSD_PICTURE pic;
    pic.decodedDataSize = sz;
    if (host->GetBuffer(host_instance_, pic))
    {
      CdmFixedBuffer *buf = new CdmFixedBuffer;
      buf->initialize(host_instance_, pic.decodedData, pic.decodedDataSize, pic.buffer);
      return buf;
    }
    return nullptr;
  };

  void insertssd(WV_CencSingleSampleDecrypter* ssd) { ssds.push_back(ssd); };
  void removessd(WV_CencSingleSampleDecrypter* ssd)
  {
    std::vector<WV_CencSingleSampleDecrypter*>::iterator res(std::find(ssds.begin(), ssds.end(), ssd));
    if (res != ssds.end())
      ssds.erase(res);
  };

  media::CdmAdapter *GetCdmAdapter() { return wv_adapter.get(); };
  const std::string &GetLicenseURL() { return license_url_; };

  cdm::Status DecryptAndDecodeFrame(void* hostInstance, cdm::InputBuffer_2 &cdm_in, media::CdmVideoFrame *frame)
  {
    host_instance_ = hostInstance;
    cdm::Status ret = wv_adapter->DecryptAndDecodeFrame(cdm_in, frame);
    host_instance_ = nullptr;
    return ret;
  }

private:
  std::shared_ptr<media::CdmAdapter> wv_adapter;
  std::string license_url_;
  void *host_instance_;

  std::vector<WV_CencSingleSampleDecrypter*> ssds;
};

WV_DRM::WV_DRM(const char* licenseURL, const AP4_DataBuffer &serverCert, const uint8_t config)
  : license_url_(licenseURL)
  , host_instance_(0)
{
  std::string strLibPath = host->GetLibraryPath();
  if (strLibPath.empty())
  {
    Log(SSD_HOST::LL_ERROR, "Absolute path to widevine in settings expected");
    return;
  }
  strLibPath += WIDEVINECDMFILENAME;

  std::string strBasePath = host->GetProfilePath();
  char cSep = strBasePath.back();
  strBasePath += "widevine";
  strBasePath += cSep;
  host->CreateDir(strBasePath.c_str());

  //Build up a CDM path to store decrypter specific stuff. Each domain gets it own path
  const char* bspos(strchr(license_url_.c_str(), ':'));
  if (!bspos || bspos[1] != '/' || bspos[2] != '/' || !(bspos = strchr(bspos + 3, '/')))
  {
    Log(SSD_HOST::LL_ERROR, "Could not find protocol inside url - invalid");
    return;
  }
  if (bspos - license_url_.c_str() > 256)
  {
    Log(SSD_HOST::LL_ERROR, "Length of domain exeeds max. size of 256 - invalid");
    return;
  }
  char buffer[1024];
  buffer[(bspos - license_url_.c_str()) * 2] = 0;
  AP4_FormatHex(reinterpret_cast<const uint8_t*>(license_url_.c_str()), bspos - license_url_.c_str(), buffer);

  strBasePath += buffer;
  strBasePath += cSep;
  host->CreateDir(strBasePath.c_str());

  wv_adapter = std::shared_ptr<media::CdmAdapter>(new media::CdmAdapter(
    "com.widevine.alpha",
    strLibPath,
    strBasePath,
    media::CdmConfig(false, (config & SSD::SSD_DECRYPTER::CONFIG_PERSISTENTSTORAGE) != 0),
    dynamic_cast<media::CdmAdapterClient*>(this)));
  if (!wv_adapter->valid())
  {
    Log(SSD_HOST::LL_ERROR, "Unable to load widevine shared library (%s)", strLibPath.c_str());
    wv_adapter = nullptr;
    return;
  }

  if (serverCert.GetDataSize())
    wv_adapter->SetServerCertificate(0, serverCert.GetData(), serverCert.GetDataSize());

  // For backward compatibility: If no | is found in URL, make the amazon convention out of it
  if (license_url_.find('|') == std::string::npos)
    license_url_ += "|Content-Type=application%2Fx-www-form-urlencoded|widevine2Challenge=B{SSM}&includeHdcpTestKeyInLicense=true|JBlicense;hdcpEnforcementResolutionPixels";

  //wv_adapter->GetStatusForPolicy();
  //wv_adapter->QueryOutputProtectionStatus();
}

WV_DRM::~WV_DRM()
{
  if (wv_adapter)
  {
    wv_adapter->RemoveClient();
    wv_adapter = nullptr;
  }
}

void WV_DRM::OnCDMMessage(const char* session, uint32_t session_size, CDMADPMSG msg, const uint8_t *data, size_t data_size, uint32_t status)
{
  Log(SSD_HOST::LL_DEBUG, "CDMMessage: %u arrived!", msg);
  std::vector<WV_CencSingleSampleDecrypter*>::iterator b(ssds.begin()), e(ssds.end());
  for (; b != e; ++b)
    if (!(*b)->GetSessionId() || strncmp((*b)->GetSessionId(), session, session_size) == 0)
      break;

  if (b == ssds.end())
    return;

  if (msg == CDMADPMSG::kSessionMessage)
  {
    (*b)->SetSession(session, session_size, data, data_size);
    (*b)->SetSessionActive();
  }
  else if (msg == CDMADPMSG::kSessionKeysChange)
    (*b)->AddSessionKey(data, data_size, status);
};

/*----------------------------------------------------------------------
|   WV_CencSingleSampleDecrypter::WV_CencSingleSampleDecrypter
+---------------------------------------------------------------------*/

WV_CencSingleSampleDecrypter::WV_CencSingleSampleDecrypter(WV_DRM &drm, AP4_DataBuffer &pssh, const uint8_t *defaultKeyId, bool skipSessionMessage)
  : AP4_CencSingleSampleDecrypter(0)
  , drm_(drm)
  , pssh_(pssh)
  , hdcp_version_(99)
  , hdcp_limit_(0)
  , resolution_limit_ (0)
  , max_subsample_count_decrypt_(0)
  , max_subsample_count_video_(0)
  , subsample_buffer_decrypt_(0)
  , subsample_buffer_video_(0)
  , promise_id_(1)
  , drained_(true)
{
  SetParentIsOwner(false);

  if (pssh.GetDataSize() > 4096)
  {
    Log(SSD_HOST::LL_ERROR, "Init_data with length: %u seems not to be cenc init data!", pssh.GetDataSize());
    return;
  }

  drm_.insertssd(this);

  if (defaultKeyId)
    memcpy(defaultKeyId_, defaultKeyId, 16);
  else
    memset(defaultKeyId_, 0, 16);

#ifdef LOCLICENSE
  std::string strDbg = host->GetProfilePath();
  strDbg += "EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED.init";
  FILE*f = fopen(strDbg.c_str(), "wb");
  if (f) {
    fwrite(pssh.GetData(), 1, pssh.GetDataSize(), f);
    fclose(f);
  }
  else
    Log(SSD_HOST::LL_DEBUG, "%s: could not open debug file for writing (init)!", __func__);
#endif

  if (memcmp(pssh.GetData() + 4, "pssh", 4) != 0)
  {
    unsigned int buf_size = 32 + pssh.GetDataSize();
    uint8_t buf[4096 + 32];

    // This will request a new session and initializes session_id and message members in cdm_adapter.
    // message will be used to create a license request in the step after CreateSession call.
    // Initialization data is the widevine cdm pssh code in google proto style found in mpd schemeIdUri
    static uint8_t proto[] = { 0x00, 0x00, 0x00, 0x63, 0x70, 0x73, 0x73, 0x68, 0x00, 0x00, 0x00, 0x00, 0xed, 0xef, 0x8b, 0xa9,
      0x79, 0xd6, 0x4a, 0xce, 0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed, 0x00, 0x00, 0x00, 0x00 };

    proto[2] = static_cast<uint8_t>((buf_size >> 8) & 0xFF);
    proto[3] = static_cast<uint8_t>(buf_size & 0xFF);
    proto[30] = static_cast<uint8_t>((pssh.GetDataSize() >> 8) & 0xFF);
    proto[31] = static_cast<uint8_t>(pssh.GetDataSize());

    memcpy(buf, proto, sizeof(proto));
    memcpy(&buf[32], pssh.GetData(), pssh.GetDataSize());
    pssh_.SetData(buf, buf_size);
  }

  drm.GetCdmAdapter()->CreateSessionAndGenerateRequest(promise_id_++, cdm::SessionType::kTemporary, cdm::InitDataType::kCenc,
    reinterpret_cast<const uint8_t *>(pssh_.GetData()), pssh_.GetDataSize());

  int retrycount=0;
  while (!drm.GetCdmAdapter()->IsSessionActive() && ++retrycount < 100)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  if (session_.empty())
  {
    Log(SSD_HOST::LL_ERROR, "License update not successful (no session)");
    return;
  }

  if (skipSessionMessage)
    return;

  while (challenge_.GetDataSize() > 0 && SendSessionMessage());

  if (keys_.empty())
  {
    Log(SSD_HOST::LL_ERROR, "License update not successful (no keys)");
    CloseSessionId();
    return;
  }
  Log(SSD_HOST::LL_DEBUG, "License update successful");
}

WV_CencSingleSampleDecrypter::~WV_CencSingleSampleDecrypter()
{
  drm_.removessd(this);
  free(subsample_buffer_decrypt_);
  free(subsample_buffer_video_);
}

void WV_CencSingleSampleDecrypter::GetCapabilities(const uint8_t* key, uint32_t media, SSD_DECRYPTER::SSD_CAPS &caps)
{
  caps = { 0, hdcp_version_, hdcp_limit_ };

  if (session_.empty())
    return;

  caps.flags = SSD_DECRYPTER::SSD_CAPS::SSD_SUPPORTS_DECODING;

  if (keys_.empty())
    return;

  if (!caps.hdcpLimit)
    caps.hdcpLimit = resolution_limit_;

  //caps.flags |= (SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH | SSD_DECRYPTER::SSD_CAPS::SSD_ANNEXB_REQUIRED);
  //return;

  /*for (auto k : keys_)
    if (!key || memcmp(k.keyid.data(), key, 16) == 0)
    {
      if (k.status != 0)
      {
        if (media == SSD_DECRYPTER::SSD_CAPS::SSD_MEDIA_VIDEO)
          caps.flags |= (SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH | SSD_DECRYPTER::SSD_CAPS::SSD_ANNEXB_REQUIRED);
        else
          caps.flags = SSD_DECRYPTER::SSD_CAPS::SSD_INVALID;
      }
      break;
    }
    */
  if (caps.flags == SSD_DECRYPTER::SSD_CAPS::SSD_SUPPORTS_DECODING)
  {
    AP4_UI32 poolid(AddPool());
    fragment_pool_[poolid].key_ = key ? key : reinterpret_cast<const uint8_t*>(keys_.front().keyid.data());

    AP4_DataBuffer in, out;
    AP4_UI32 encb[2] = { 1,1 };
    AP4_UI16 clearb[2] = { 5,5 };
    AP4_Byte vf[12]={0,0,0,1,9,255,0,0,0,1,10,255};
    const AP4_UI08 iv[] = { 1,2,3,4,5,6,7,8,0,0,0,0,0,0,0,0 };
    in.SetBuffer(vf,12);
    in.SetDataSize(12);
    try {
      if (DecryptSampleData(poolid, in, out, iv, 2, clearb, encb) != AP4_SUCCESS)
      {
        encb[0] = 12;
        clearb[0] = 0;
        if (DecryptSampleData(poolid, in, out, iv, 1, clearb, encb) != AP4_SUCCESS)
        {
          if (media == SSD_DECRYPTER::SSD_CAPS::SSD_MEDIA_VIDEO)
            caps.flags |= (SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH | SSD_DECRYPTER::SSD_CAPS::SSD_ANNEXB_REQUIRED);
          else
            caps.flags = SSD_DECRYPTER::SSD_CAPS::SSD_INVALID;
        }
        else
        {
          caps.flags |= SSD_DECRYPTER::SSD_CAPS::SSD_SINGLE_DECRYPT;
          caps.hdcpVersion = 99;
          caps.hdcpLimit = resolution_limit_;
        }
      }
      else
      {
        caps.hdcpVersion = 99;
        caps.hdcpLimit = resolution_limit_;
      }
    }
    catch (...) {
      caps.flags |= (SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH | SSD_DECRYPTER::SSD_CAPS::SSD_ANNEXB_REQUIRED);
    }
    RemovePool(poolid);
  }
}

const char *WV_CencSingleSampleDecrypter::GetSessionId()
{
  return session_.empty()? nullptr : session_.c_str();
}

void WV_CencSingleSampleDecrypter::SetSessionActive()
{
  drm_.GetCdmAdapter()->SetSessionActive(true);
}

void WV_CencSingleSampleDecrypter::CloseSessionId()
{
  if (!session_.empty())
  {
    Log(SSD_HOST::LL_DEBUG, "%s: close session with Id: %s", __func__, session_.c_str());
    drm_.GetCdmAdapter()->CloseSession(++promise_id_, session_.data(), session_.size());
    session_.clear();

    Log(SSD_HOST::LL_DEBUG, "%s: session closed", __func__);
  }
}

AP4_DataBuffer WV_CencSingleSampleDecrypter::GetChallengeData()
{
  return challenge_;
}

void WV_CencSingleSampleDecrypter::CheckLicenseRenewal()
{
  {
    std::lock_guard<std::mutex> lock(renewal_lock_);
    if (!challenge_.GetDataSize())
      return;
  }
  SendSessionMessage();
}

bool WV_CencSingleSampleDecrypter::SendSessionMessage()
{
  std::vector<std::string> headers, header, blocks = split(drm_.GetLicenseURL(), '|');
  if (blocks.size() != 4)
  {
    Log(SSD_HOST::LL_ERROR, "4 '|' separated blocks in licURL expected (req / header / body / response)");
    return false;
  }

#ifdef LOCLICENSE
  std::string strDbg = host->GetProfilePath();
  strDbg += "EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED.challenge";
  FILE*f = fopen(strDbg.c_str(), "wb");
  if (f) {
    fwrite(challenge_.GetData(), 1, challenge_.GetDataSize(), f);
    fclose(f);
  }
  else
    Log(SSD_HOST::LL_DEBUG, "%s: could not open debug file for writing (challenge)!", __func__);
#endif

  //Process placeholder in GET String
  std::string::size_type insPos(blocks[0].find("{SSM}"));
  if (insPos != std::string::npos)
  {
    if (insPos > 0 && blocks[0][insPos - 1] == 'B')
    {
      std::string msgEncoded = b64_encode(challenge_.GetData(), challenge_.GetDataSize(), true);
      blocks[0].replace(insPos - 1, 6, msgEncoded);
    }
    else
    {
      Log(SSD_HOST::LL_ERROR, "Unsupported License request template (cmd)");
      return false;
    }
  }

  insPos = blocks[0].find("{HASH}");
  if (insPos != std::string::npos)
  {
    MD5 md5;
    md5.update(challenge_.GetData(), challenge_.GetDataSize());
    md5.finalize();
    blocks[0].replace(insPos, 6, md5.hexdigest());
  }

  void* file = host->CURLCreate(blocks[0].c_str());

  size_t nbRead;
  std::string response, resLimit, contentType;
  char buf[2048];
  bool serverCertRequest;

  //Set our std headers
  host->CURLAddOption(file, SSD_HOST::OPTION_PROTOCOL, "acceptencoding", "gzip, deflate");
  host->CURLAddOption(file, SSD_HOST::OPTION_PROTOCOL, "seekable", "0");
  host->CURLAddOption(file, SSD_HOST::OPTION_HEADER, "Expect", "");

  //Process headers
  headers = split(blocks[1], '&');
  for (std::vector<std::string>::iterator b(headers.begin()), e(headers.end()); b != e; ++b)
  {
    header = split(*b, '=');
    host->CURLAddOption(file, SSD_HOST::OPTION_PROTOCOL, trim(header[0]).c_str(), header.size() > 1 ? url_decode(trim(header[1])).c_str() : "");
  }

  //Process body
  if (!blocks[2].empty())
  {
    if (blocks[2][0] == '%')
      blocks[2] = url_decode(blocks[2]);

    insPos = blocks[2].find("{SSM}");
    if (insPos != std::string::npos)
    {
      std::string::size_type sidPos(blocks[2].find("{SID}"));
      std::string::size_type kidPos(blocks[2].find("{KID}"));

      char fullDecode = 0;
      if (insPos > 1 && sidPos > 1 && kidPos > 1 && (blocks[2][0] == 'b' || blocks[2][0] == 'B') && blocks[2][1] == '{')
      {
        fullDecode = blocks[2][0];
        blocks[2] = blocks[2].substr(2, blocks[2].size() - 3);
        insPos -= 2;
        if (kidPos != std::string::npos)
          kidPos -= 2;
        if (sidPos != std::string::npos)
          sidPos -= 2;
      }

      size_t size_written(0);

      if (insPos > 0)
      {
        if (blocks[2][insPos - 1] == 'B' || blocks[2][insPos - 1] == 'b')
        {
          std::string msgEncoded = b64_encode(challenge_.GetData(), challenge_.GetDataSize(), blocks[2][insPos - 1] == 'B');
          blocks[2].replace(insPos - 1, 6, msgEncoded);
          size_written = msgEncoded.size();
        }
        else if (blocks[2][insPos - 1] == 'D')
        {
          std::string msgEncoded = ToDecimal(challenge_.GetData(), challenge_.GetDataSize());
          blocks[2].replace(insPos - 1, 6, msgEncoded);
          size_written = msgEncoded.size();
        }
        else
        {
          blocks[2].replace(insPos - 1, 6, reinterpret_cast<const char*>(challenge_.GetData()), challenge_.GetDataSize());
          size_written = challenge_.GetDataSize();
        }
      }
      else
      {
        Log(SSD_HOST::LL_ERROR, "Unsupported License request template (body / ?{SSM})");
        goto SSMFAIL;
      }

      if (sidPos != std::string::npos && insPos < sidPos)
        sidPos += size_written, sidPos -= 6;

      if (kidPos != std::string::npos && insPos < kidPos)
        kidPos += size_written, kidPos -= 6;

      size_written = 0;

      if (sidPos != std::string::npos)
      {
        if (sidPos > 0)
        {
          if (blocks[2][sidPos - 1] == 'B' || blocks[2][sidPos - 1] == 'b')
          {
            std::string msgEncoded = b64_encode(reinterpret_cast<const unsigned char*>(session_.data()),session_.size(), blocks[2][sidPos - 1] == 'B');
            blocks[2].replace(sidPos - 1, 6, msgEncoded);
            size_written = msgEncoded.size();
          }
          else
          {
            blocks[2].replace(sidPos - 1, 6, session_.data(), session_.size());
            size_written = session_.size();
          }
        }
        else
        {
          Log(SSD_HOST::LL_ERROR, "Unsupported License request template (body / ?{SID})");
          goto SSMFAIL;
        }
      }

      if (kidPos != std::string::npos)
      {
        if (sidPos < kidPos)
          kidPos += size_written, kidPos -= 6;
        char uuid[36];
        if (blocks[2][kidPos - 1] == 'H')
        {
          AP4_FormatHex(defaultKeyId_, 16, uuid);
          blocks[2].replace(kidPos - 1, 6, (const char*)uuid, 32);
        }
        else
        {
          KIDtoUUID(defaultKeyId_, uuid);
          blocks[2].replace(kidPos, 5, (const char*)uuid, 36);
        }
      }

      if (fullDecode)
        blocks[2] = b64_encode(reinterpret_cast<const unsigned char*>(blocks[2].data()), blocks[2].size(), fullDecode == 'B');
    }
    std::string decoded = b64_encode(reinterpret_cast<const unsigned char*>(blocks[2].data()), blocks[2].size(), false);
    host->CURLAddOption(file, SSD_HOST::OPTION_PROTOCOL, "postdata", decoded.c_str());
  }

  serverCertRequest = challenge_.GetDataSize() == 2;
  challenge_.SetDataSize(0);

  if (!host->CURLOpen(file))
  {
    Log(SSD_HOST::LL_ERROR, "License server returned failure");
    goto SSMFAIL;
  }

  // read the file
  while ((nbRead = host->ReadFile(file, buf, 1024)) > 0)
    response += std::string((const char*)buf, nbRead);

  resLimit = host->CURLGetProperty(file, SSD_HOST::CURLPROPERTY::PROPERTY_HEADER, "X-Limit-Video");
  contentType = host->CURLGetProperty(file, SSD_HOST::CURLPROPERTY::PROPERTY_HEADER, "Content-Type");

  if (!resLimit.empty())
  {
    std::string::size_type posMax = resLimit.find("max=");
    if (posMax != std::string::npos)
      resolution_limit_ = atoi(resLimit.data() + (posMax + 4));
  }

  host->CloseFile(file);
  file = 0;

  if (nbRead != 0)
  {
    Log(SSD_HOST::LL_ERROR, "Could not read full SessionMessage response");
    goto SSMFAIL;
  }

#ifdef LOCLICENSE
  strDbg = host->GetProfilePath();
  strDbg += "EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED.response";
  f = fopen(strDbg.c_str(), "wb");
  if (f) {
    fwrite(response.c_str(), 1, response.size(), f);
    fclose(f);
  }
  else
    Log(SSD_HOST::LL_DEBUG, "%s: could not open debug file for writing (response)!", __func__);
#endif

  if (serverCertRequest && contentType.find("application/octet-stream") == std::string::npos)
    serverCertRequest = false;

  if (!blocks[3].empty() && !serverCertRequest)
  {
    if (blocks[3][0] == 'J' || (blocks[3].size() > 1 && blocks[3][0] == 'B' && blocks[3][1] == 'J'))
    {
      int dataPos = 2;

      if (response.size() >= 3 && blocks[3][0] == 'B')
      {
        unsigned int decoded_size = 2048;
        uint8_t decoded[2048];
        b64_decode(response.c_str(), response.size(), decoded, decoded_size);
        response = std::string(reinterpret_cast<const char*>(decoded), decoded_size);
        dataPos = 3;
      }

      jsmn_parser jsn;
      jsmntok_t tokens[256];

      jsmn_init(&jsn);
      int i(0), numTokens = jsmn_parse(&jsn, response.c_str(), response.size(), tokens, 256);

      std::vector<std::string> jsonVals = split(blocks[3].c_str() + dataPos, ';');

      // Find HDCP limit
      if (jsonVals.size() > 1)
      {
        for (; i < numTokens; ++i)
          if (tokens[i].type == JSMN_STRING && tokens[i].size == 1 && jsonVals[1].size() == static_cast<unsigned int>(tokens[i].end - tokens[i].start)
            && strncmp(response.c_str() + tokens[i].start, jsonVals[1].c_str(), tokens[i].end - tokens[i].start) == 0)
            break;
        if (i < numTokens)
          hdcp_limit_ = atoi((response.c_str() + tokens[i + 1].start));
      }
      // Find license key
      if (jsonVals.size() > 0)
      {
        for (i = 0; i < numTokens; ++i)
          if (tokens[i].type == JSMN_STRING && tokens[i].size == 1 && jsonVals[0].size() == static_cast<unsigned int>(tokens[i].end - tokens[i].start)
            && strncmp(response.c_str() + tokens[i].start, jsonVals[0].c_str(), tokens[i].end - tokens[i].start) == 0)
          {
            if (i + 1 < numTokens && tokens[i + 1].type == JSMN_ARRAY && tokens[i + 1].size == 1)
              ++i;
            break;
          }
      }
      else
        i = numTokens;

      if (i < numTokens)
      {
        if (blocks[3][dataPos - 1] == 'B')
        {
          unsigned int decoded_size = 2048;
          uint8_t decoded[2048];
          b64_decode(response.c_str() + tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start, decoded, decoded_size);
          drm_.GetCdmAdapter()->UpdateSession(++promise_id_, session_.data(), session_.size(), reinterpret_cast<const uint8_t*>(decoded), decoded_size);
        }
        else
          drm_.GetCdmAdapter()->UpdateSession(++promise_id_, session_.data(), session_.size(),
            reinterpret_cast<const uint8_t*>(response.c_str() + tokens[i + 1].start), tokens[i + 1].end - tokens[i + 1].start);
      }
      else
      {
        Log(SSD_HOST::LL_ERROR, "Unable to find %s in JSON string", blocks[3].c_str() + 2);
        goto SSMFAIL;
      }
    }
    else if (blocks[3][0] == 'H' && blocks[3].size() >= 2)
    {
      //Find the payload
      std::string::size_type payloadPos = response.find("\r\n\r\n");
      if (payloadPos != std::string::npos)
      {
        payloadPos += 4;
        if (blocks[3][1] == 'B')
          drm_.GetCdmAdapter()->UpdateSession(++promise_id_, session_.data(), session_.size(),
            reinterpret_cast<const uint8_t*>(response.c_str() + payloadPos), response.size() - payloadPos);
        else
        {
          Log(SSD_HOST::LL_ERROR, "Unsupported HTTP payload data type definition");
          goto SSMFAIL;
        }
      }
      else
      {
        Log(SSD_HOST::LL_ERROR, "Unable to find HTTP payload in response");
        goto SSMFAIL;
      }
    }
    else if (blocks[3][0] == 'B' && blocks[3].size() == 1)
    {
      unsigned int decoded_size = 2048;
      uint8_t decoded[2048];
      b64_decode(response.c_str(), response.size(), decoded, decoded_size);
      drm_.GetCdmAdapter()->UpdateSession(++promise_id_, session_.data(), session_.size(),
        reinterpret_cast<const uint8_t*>(decoded), decoded_size);
    }
    else
    {
      Log(SSD_HOST::LL_ERROR, "Unsupported License request template (response)");
      goto SSMFAIL;
    }
  } else //its binary - simply push the returned data as update
    drm_.GetCdmAdapter()->UpdateSession(++promise_id_, session_.data(), session_.size(),
      reinterpret_cast<const uint8_t*>(response.data()), response.size());

  return true;
SSMFAIL:
  if (file)
    host->CloseFile(file);
  return false;
}

void WV_CencSingleSampleDecrypter::AddSessionKey(const uint8_t *data, size_t data_size, uint32_t status)
{
  WVSKEY key;
  std::vector<WVSKEY>::iterator res;

  key.keyid = std::string((const char*)data, data_size);
  if ((res = std::find(keys_.begin(), keys_.end(), key)) == keys_.end())
    res = keys_.insert(res, key);
  res->status = static_cast<cdm::KeyStatus>(status);
}

/*----------------------------------------------------------------------
|   WV_CencSingleSampleDecrypter::SetKeyId
+---------------------------------------------------------------------*/

bool WV_CencSingleSampleDecrypter::HasKeyId(const uint8_t *keyid)
{
  if (keyid)
    for (std::vector<WVSKEY>::const_iterator kb(keys_.begin()), ke(keys_.end()); kb != ke; ++kb)
      if (kb->keyid.size() == 16 && memcmp(kb->keyid.c_str(), keyid, 16) == 0)
        return true;
  return false;
}

AP4_Result WV_CencSingleSampleDecrypter::SetFragmentInfo(AP4_UI32 pool_id, const AP4_UI08 *key, const AP4_UI08 nal_length_size, AP4_DataBuffer &annexb_sps_pps, AP4_UI32 flags)
{
  if (pool_id >= fragment_pool_.size())
    return AP4_ERROR_OUT_OF_RANGE;

  fragment_pool_[pool_id].key_ = key;
  fragment_pool_[pool_id].nal_length_size_ = nal_length_size;
  fragment_pool_[pool_id].annexb_sps_pps_.SetData(annexb_sps_pps.GetData(), annexb_sps_pps.GetDataSize());
  fragment_pool_[pool_id].decrypter_flags_ = flags;

  return AP4_SUCCESS;
}

AP4_UI32 WV_CencSingleSampleDecrypter::AddPool()
{
  for (size_t i(0); i < fragment_pool_.size(); ++i)
    if (fragment_pool_[i].nal_length_size_ == 99)
    {
      fragment_pool_[i].nal_length_size_ = 0;
      return i;
    }
  fragment_pool_.push_back(FINFO());
  fragment_pool_.back().nal_length_size_ = 0;
  return static_cast<AP4_UI32>(fragment_pool_.size() - 1);
}


void WV_CencSingleSampleDecrypter::RemovePool(AP4_UI32 poolid)
{
  fragment_pool_[poolid].nal_length_size_ = 99;
  fragment_pool_[poolid].key_ = nullptr;
}

/*----------------------------------------------------------------------
|   WV_CencSingleSampleDecrypter::DecryptSampleData
+---------------------------------------------------------------------*/
AP4_Result WV_CencSingleSampleDecrypter::DecryptSampleData(AP4_UI32 pool_id,
  AP4_DataBuffer& data_in,
  AP4_DataBuffer& data_out,
  const AP4_UI08* iv,
  unsigned int    subsample_count,
  const AP4_UI16* bytes_of_cleartext_data,
  const AP4_UI32* bytes_of_encrypted_data)
{
  if (!drm_.GetCdmAdapter())
  {
    data_out.SetData(data_in.GetData(), data_in.GetDataSize());
    return AP4_SUCCESS;
  }

  FINFO &fragInfo(fragment_pool_[pool_id]);

  if(fragInfo.decrypter_flags_ & SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH) //we can not decrypt only
  {
    if (fragInfo.nal_length_size_ > 4)
    {
      Log(SSD_HOST::LL_ERROR, "Nalu length size > 4 not supported");
      return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_UI16 dummyClear(0);
    AP4_UI32 dummyCipher(data_in.GetDataSize());

    if (iv)
    {
      if (!subsample_count)
      {
        subsample_count = 1;
        bytes_of_cleartext_data = &dummyClear;
        bytes_of_encrypted_data = &dummyCipher;
      }

      data_out.SetData(reinterpret_cast<const AP4_Byte*>(&subsample_count), sizeof(subsample_count));
      data_out.AppendData(reinterpret_cast<const AP4_Byte*>(bytes_of_cleartext_data), subsample_count * sizeof(AP4_UI16));
      data_out.AppendData(reinterpret_cast<const AP4_Byte*>(bytes_of_encrypted_data), subsample_count * sizeof(AP4_UI32));
      data_out.AppendData(reinterpret_cast<const AP4_Byte*>(iv), 16);
      data_out.AppendData(reinterpret_cast<const AP4_Byte*>(fragInfo.key_), 16);
    }
    else
    {
      data_out.SetDataSize(0);
      bytes_of_cleartext_data = &dummyClear;
      bytes_of_encrypted_data = &dummyCipher;
    }

    if (fragInfo.nal_length_size_ && (!iv || bytes_of_cleartext_data[0] > 0))
    {
      //Note that we assume that there is enough data in data_out to hold everything without reallocating.

      //check NAL / subsample
      const AP4_Byte *packet_in(data_in.GetData()), *packet_in_e(data_in.GetData() + data_in.GetDataSize());
      AP4_Byte *packet_out(data_out.UseData() + data_out.GetDataSize());
      AP4_UI16 *clrb_out(iv ? reinterpret_cast<AP4_UI16*>(data_out.UseData() + sizeof(subsample_count)):nullptr);
      unsigned int nalunitcount(0), nalunitsum(0), configSize(0);

      while (packet_in < packet_in_e)
      {
        uint32_t nalsize(0);
        for (unsigned int i(0); i < fragInfo.nal_length_size_; ++i) { nalsize = (nalsize << 8) + *packet_in++; };

        //look if we have to inject sps / pps
        if (fragInfo.annexb_sps_pps_.GetDataSize() && (*packet_in & 0x1F) != 9 /*AVC_NAL_AUD*/)
        {
          memcpy(packet_out, fragInfo.annexb_sps_pps_.GetData(), fragInfo.annexb_sps_pps_.GetDataSize());
          packet_out += fragInfo.annexb_sps_pps_.GetDataSize();
          if(clrb_out) *clrb_out += fragInfo.annexb_sps_pps_.GetDataSize();
          configSize = fragInfo.annexb_sps_pps_.GetDataSize();
          fragInfo.annexb_sps_pps_.SetDataSize(0);
        }

        //Anex-B Start pos
        packet_out[0] = packet_out[1] = packet_out[2] = 0; packet_out[3] = 1;
        packet_out += 4;
        memcpy(packet_out, packet_in, nalsize);
        packet_in += nalsize;
        packet_out += nalsize;
        if (clrb_out) *clrb_out += (4 - fragInfo.nal_length_size_);
        ++nalunitcount;

        if (!iv)
        {
          nalunitsum = 0;
        }
        else if (nalsize + fragInfo.nal_length_size_ + nalunitsum >= *bytes_of_cleartext_data + *bytes_of_encrypted_data)
        {
          AP4_UI32 summedBytes(0);
          do
          {
            summedBytes += *bytes_of_cleartext_data + *bytes_of_encrypted_data;
            ++bytes_of_cleartext_data;
            ++bytes_of_encrypted_data;
            ++clrb_out;
            --subsample_count;
          } while (subsample_count && nalsize + fragInfo.nal_length_size_ + nalunitsum > summedBytes);

          if (nalsize + fragInfo.nal_length_size_ + nalunitsum > summedBytes)
          {
            Log(SSD_HOST::LL_ERROR, "NAL Unit exceeds subsample definition (nls: %u) %u -> %u ",
              static_cast<unsigned int>(fragInfo.nal_length_size_),
              static_cast<unsigned int>(nalsize + fragInfo.nal_length_size_ + nalunitsum),
              summedBytes);
            return AP4_ERROR_NOT_SUPPORTED;
          }
          nalunitsum = 0;
        }
        else
          nalunitsum += nalsize + fragInfo.nal_length_size_;
      }
      if (packet_in != packet_in_e || subsample_count)
      {
        Log(SSD_HOST::LL_ERROR, "NAL Unit definition incomplete (nls: %u) %u -> %u ",
          static_cast<unsigned int>(fragInfo.nal_length_size_),
          static_cast<unsigned int>(packet_in_e - packet_in),
          subsample_count);
        return AP4_ERROR_NOT_SUPPORTED;
      }
      data_out.SetDataSize(data_out.GetDataSize() + data_in.GetDataSize() + configSize + (4 - fragInfo.nal_length_size_) * nalunitcount);
    }
    else
      data_out.AppendData(data_in.GetData(), data_in.GetDataSize());
    return AP4_SUCCESS;
  }

  if (!fragInfo.key_)
  {
    Log(SSD_HOST::LL_DEBUG, "DecryptSampleData: No Key");
    return AP4_ERROR_INVALID_PARAMETERS;
  }

  // the output has the same size as the input
  data_out.SetDataSize(data_in.GetDataSize());

  uint16_t clearb(0);
  uint32_t cipherb(data_in.GetDataSize());

  // check input parameters
  if (iv == NULL) return AP4_ERROR_INVALID_PARAMETERS;
  if (subsample_count) {
    if (bytes_of_cleartext_data == NULL || bytes_of_encrypted_data == NULL)
    {
      Log(SSD_HOST::LL_DEBUG, "DecryptSampleData: inputparams invalid");
      return AP4_ERROR_INVALID_PARAMETERS;
    }
  }
  else
  {
    subsample_count = 1;
    bytes_of_cleartext_data = &clearb;
    bytes_of_encrypted_data = &cipherb;
  }

  // transform ap4 format into cmd format
  cdm::InputBuffer_2 cdm_in;
  if (subsample_count > max_subsample_count_decrypt_)
  {
    subsample_buffer_decrypt_ = (cdm::SubsampleEntry*)realloc(subsample_buffer_decrypt_, subsample_count*sizeof(cdm::SubsampleEntry));
    max_subsample_count_decrypt_ = subsample_count;
  }

  bool useSingleDecrypt(false);

  // CDM should get 1 block of encrypted data per sample, encrypted data
  // from all subsamples should be formed into a contiguous block.
  // Even if there is only 1 subsample, we should remove cleartext data
  // from it before passing to CDM.
  if ((fragInfo.decrypter_flags_ & SSD_DECRYPTER::SSD_CAPS::SSD_SINGLE_DECRYPT) != 0)
  {
    decrypt_in_.Reserve(data_in.GetDataSize());
    decrypt_in_.SetDataSize(0);
    size_t absPos = 0;

    for (unsigned int i(0); i < subsample_count; ++i)
    {
      absPos += bytes_of_cleartext_data[i];
      decrypt_in_.AppendData(data_in.GetData() + absPos, bytes_of_encrypted_data[i]);
      absPos += bytes_of_encrypted_data[i];
    }
    if (decrypt_in_.GetDataSize())
    {
      decrypt_out_.SetDataSize(decrypt_in_.GetDataSize());
      subsample_buffer_decrypt_[0].clear_bytes = 0;
      subsample_buffer_decrypt_[0].cipher_bytes = decrypt_in_.GetDataSize();
      cdm_in.data = decrypt_in_.GetData();
      cdm_in.data_size = decrypt_in_.GetDataSize();
      cdm_in.num_subsamples = 1;
      useSingleDecrypt = true;
    }
  }

  if (!useSingleDecrypt)
  {
    unsigned int i(0), numCipherBytes(0);
    for (cdm::SubsampleEntry *b(subsample_buffer_decrypt_), *e(subsample_buffer_decrypt_ + subsample_count); b != e; ++b, ++i)
    {
      b->clear_bytes = bytes_of_cleartext_data[i];
      b->cipher_bytes = bytes_of_encrypted_data[i];
      numCipherBytes += b->cipher_bytes;
    }
    if (numCipherBytes)
    {
      cdm_in.data = data_in.GetData();
      cdm_in.data_size = data_in.GetDataSize();
      cdm_in.num_subsamples = subsample_count;
    }
    else
    {
      memcpy(data_out.UseData(), data_in.GetData(), data_in.GetDataSize());
      return AP4_SUCCESS;
    }
  }

  cdm_in.iv = iv;
  cdm_in.iv_size = 16; //Always 16, see AP4_CencSingleSampleDecrypter declaration.
  cdm_in.key_id = fragInfo.key_;
  cdm_in.key_id_size = 16;
  cdm_in.subsamples = subsample_buffer_decrypt_;
  cdm_in.encryption_scheme = cdm::EncryptionScheme::kCenc;
  cdm_in.timestamp = 0;
  cdm_in.pattern = { 0,0 };

  CdmBuffer buf((useSingleDecrypt) ? &decrypt_out_ : &data_out);
  CdmDecryptedBlock cdm_out;
  cdm_out.SetDecryptedBuffer(&buf);

  //LICENSERENEWAL: 
  CheckLicenseRenewal();
  cdm::Status ret = drm_.GetCdmAdapter()->Decrypt(cdm_in, &cdm_out);

  if (ret == cdm::Status::kSuccess && useSingleDecrypt)
  {
    size_t absPos = 0, cipherPos = 0;
    for (unsigned int i(0); i < subsample_count; ++i)
    {
      memcpy(data_out.UseData() + absPos, data_in.GetData() + absPos, bytes_of_cleartext_data[i]);
      absPos += bytes_of_cleartext_data[i];
      memcpy(data_out.UseData() + absPos, decrypt_out_.GetData() + cipherPos, bytes_of_encrypted_data[i]);
      absPos += bytes_of_encrypted_data[i], cipherPos += bytes_of_encrypted_data[i];
    }
  }

  if (ret != cdm::Status::kSuccess)
  {
    char buf[36]; buf[32] = 0;
    AP4_FormatHex(fragInfo.key_, 16, buf);
    Log(SSD_HOST::LL_DEBUG, "DecryptSampleData: Decrypt failed with error: %d and key: %s", ret, buf);
  }

  return (ret == cdm::Status::kSuccess) ? AP4_SUCCESS : AP4_ERROR_INVALID_PARAMETERS;
}

bool WV_CencSingleSampleDecrypter::OpenVideoDecoder(const SSD_VIDEOINITDATA *initData)
{
  cdm::VideoDecoderConfig_3 vconfig;
  vconfig.codec = static_cast<cdm::VideoCodec>(initData->codec);
  vconfig.coded_size.width = initData->width;
  vconfig.coded_size.height = initData->height;
  vconfig.extra_data = const_cast<uint8_t*>(initData->extraData);
  vconfig.extra_data_size = initData->extraDataSize;
  vconfig.format = static_cast<cdm::VideoFormat> (initData->videoFormats[0]);
  vconfig.profile = static_cast<cdm::VideoCodecProfile>(initData->codecProfile);
  vconfig.color_space = { 2, 2, 2, cdm::ColorRange::kInvalid };
  vconfig.encryption_scheme = cdm::EncryptionScheme::kCenc;

  cdm::Status ret = drm_.GetCdmAdapter()->InitializeVideoDecoder(vconfig);
  videoFrames_.clear();
  drained_ = true;

  Log(SSD_HOST::LL_DEBUG, "WVDecoder initialization returned status %i", ret);

  return ret == cdm::Status::kSuccess;
}

SSD_DECODE_RETVAL WV_CencSingleSampleDecrypter::DecodeVideo(void* hostInstance, SSD_SAMPLE *sample, SSD_PICTURE *picture)
{
  if (sample)
  {
    // if we have an picture waiting, or not yet get the dest buffer, do nothing
    if (videoFrames_.size() == 4)
      return VC_ERROR;

    cdm::InputBuffer_2 cdm_in;

    if (sample->numSubSamples) {
      if (sample->clearBytes == NULL || sample->cipherBytes == NULL) {
        return VC_ERROR;
      }
    }

    // transform ap4 format into cmd format
    if (sample->numSubSamples > max_subsample_count_video_)
    {
      subsample_buffer_video_ = (cdm::SubsampleEntry*)realloc(subsample_buffer_video_, sample->numSubSamples * sizeof(cdm::SubsampleEntry));
      max_subsample_count_video_ = sample->numSubSamples;
    }
    cdm_in.num_subsamples = sample->numSubSamples;
    cdm_in.subsamples = subsample_buffer_video_;

    const uint16_t *clearBytes(sample->clearBytes);
    const uint32_t *cipherBytes(sample->cipherBytes);

    for (cdm::SubsampleEntry *b(subsample_buffer_video_), *e(subsample_buffer_video_ + sample->numSubSamples); b != e; ++b, ++clearBytes, ++cipherBytes)
    {
      b->clear_bytes = *clearBytes;
      b->cipher_bytes = *cipherBytes;
    }
    cdm_in.data = sample->data;
    cdm_in.data_size = sample->dataSize;
    cdm_in.iv = sample->iv;
    cdm_in.iv_size = sample->iv ? 16 : 0;
    cdm_in.timestamp = sample->pts;
    cdm_in.encryption_scheme = sample->kid ? cdm::EncryptionScheme::kCenc : cdm::EncryptionScheme::kUnencrypted;
    cdm_in.pattern = { 0,0 };

    uint8_t unencryptedKID [] = { 0 };
    cdm_in.key_id = sample->kid ? sample->kid : unencryptedKID;
    cdm_in.key_id_size = sample->kid ? 16 : 0;

    if (sample->dataSize)
      drained_ = false;

    //DecryptAndDecode calls Alloc wich cals kodi VideoCodec. Set instance handle.
    //LICENSERENEWAL:
    CheckLicenseRenewal();
    media::CdmVideoFrame frame;
    cdm::Status ret = drm_.DecryptAndDecodeFrame(hostInstance, cdm_in, &frame);

    if (ret == cdm::Status::kSuccess)
    {
      std::list<media::CdmVideoFrame>::iterator f(videoFrames_.begin());
      while (f != videoFrames_.end() && f->Timestamp() < frame.Timestamp())++f;
      videoFrames_.insert(f, frame);
    }

    if (ret == cdm::Status::kSuccess || (cdm_in.data && ret == cdm::Status::kNeedMoreData))
      return VC_NONE;
    else
    {
      if (ret == cdm::Status::kNoKey)
      {
        char buf[36]; buf[0] = buf[32] = 0;
        AP4_FormatHex(cdm_in.key_id, cdm_in.key_id_size, buf);
        Log(SSD_HOST::LL_ERROR, "DecodeVideo: kNoKey for key %s", buf);
        return VC_EOF;
      }
      return VC_ERROR;
    }
  }
  else if (picture)
  {
    if (videoFrames_.size() == 4 || (videoFrames_.size() && (picture->flags & SSD_PICTURE::FLAG_DRAIN)))
    {
      media::CdmVideoFrame &videoFrame_(videoFrames_.front());

      picture->width = videoFrame_.Size().width;
      picture->height = videoFrame_.Size().height;
      picture->pts = videoFrame_.Timestamp();
      picture->decodedData = videoFrame_.FrameBuffer()->Data();
      picture->decodedDataSize = videoFrame_.FrameBuffer()->Size();
      picture->buffer = static_cast<CdmFixedBuffer*>(videoFrame_.FrameBuffer())->Buffer();

      for (unsigned int i(0); i < cdm::VideoPlane::kMaxPlanes; ++i)
      {
        picture->planeOffsets[i] = videoFrame_.PlaneOffset(static_cast<cdm::VideoPlane>(i));
        picture->stride[i] = videoFrame_.Stride(static_cast<cdm::VideoPlane>(i));
      }
      picture->videoFormat = static_cast<SSD::SSD_VIDEOFORMAT>(videoFrame_.Format());
      videoFrame_.SetFrameBuffer(nullptr); //marker for "No Picture"

      delete (CdmFixedBuffer*)(videoFrame_.FrameBuffer());
      videoFrames_.pop_front();

      return VC_PICTURE;
    }
    else if ((picture->flags & SSD_PICTURE::FLAG_DRAIN))
    {
      static SSD_SAMPLE drainSample = { nullptr,0,0,0,0,nullptr,nullptr,nullptr,nullptr };
      if (drained_ || DecodeVideo(hostInstance, &drainSample, nullptr) == VC_ERROR)
      {
        drained_ = true;
        return VC_EOF;
      }
      else
        return VC_NONE;
    }
    else
      return VC_BUFFER;
  }
  else
    return VC_ERROR;
}

void WV_CencSingleSampleDecrypter::ResetVideo()
{
  drm_.GetCdmAdapter()->ResetDecoder(cdm::kStreamTypeVideo);
  drained_ = true;
}

/*********************************************************************************************/

class WVDecrypter : public SSD_DECRYPTER
{
public:
  WVDecrypter() : cdmsession_(nullptr), decoding_decrypter_(nullptr) {};
  virtual ~WVDecrypter()
  {
    delete cdmsession_;
    cdmsession_ = nullptr;
  };

  virtual const char *SelectKeySytem(const char* keySystem) override
  {
    if (strcmp(keySystem, "com.widevine.alpha"))
      return nullptr;

    return "urn:uuid:EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED";
  }

  virtual bool OpenDRMSystem(const char *licenseURL, const AP4_DataBuffer &serverCertificate, const uint8_t config) override
  {
    cdmsession_ = new WV_DRM(licenseURL, serverCertificate, config);

    return cdmsession_->GetCdmAdapter() != nullptr;
  }

  virtual AP4_CencSingleSampleDecrypter *CreateSingleSampleDecrypter(AP4_DataBuffer &pssh, const char *optionalKeyParameter, const uint8_t *defaultkeyid, bool skipSessionMessage) override
  {
    WV_CencSingleSampleDecrypter *decrypter = new WV_CencSingleSampleDecrypter(*cdmsession_, pssh, defaultkeyid, skipSessionMessage);
    if (!decrypter->GetSessionId())
    {
      delete decrypter;
      decrypter = nullptr;
    }
    return decrypter;
  }

  virtual void DestroySingleSampleDecrypter(AP4_CencSingleSampleDecrypter* decrypter) override
  {
    if (decrypter)
    {
      // close session before dispose
      static_cast<WV_CencSingleSampleDecrypter*>(decrypter)->CloseSessionId();
      delete static_cast<WV_CencSingleSampleDecrypter*>(decrypter);
    }
  }

  virtual void GetCapabilities(AP4_CencSingleSampleDecrypter* decrypter, const uint8_t *keyid, uint32_t media, SSD_DECRYPTER::SSD_CAPS &caps) override
  {
    if (!decrypter)
    {
      caps = { 0,0,0 };
      return;
    }

    static_cast<WV_CencSingleSampleDecrypter*>(decrypter)->GetCapabilities(keyid, media, caps);
  }

  virtual bool HasLicenseKey(AP4_CencSingleSampleDecrypter* decrypter, const uint8_t *keyid) override
  {
    if (decrypter)
      return static_cast<WV_CencSingleSampleDecrypter*>(decrypter)->HasKeyId(keyid);
    return false;
  }

  virtual bool HasCdmSession() 
  { 
    return cdmsession_ != nullptr; 
  }

  virtual std::string GetChallengeB64Data(AP4_CencSingleSampleDecrypter* decrypter) override
  {
    if (!decrypter)
      return nullptr;

    AP4_DataBuffer challengeData = static_cast<WV_CencSingleSampleDecrypter*>(decrypter)->GetChallengeData();
    // Keep b64_encode urlEncode enabled otherwise the data will not be sent correctly in the HTTP header
    return b64_encode(challengeData.GetData(), challengeData.GetDataSize(), true);
  }

  virtual bool OpenVideoDecoder(AP4_CencSingleSampleDecrypter* decrypter, const SSD_VIDEOINITDATA *initData) override
  {
    if (!decrypter || !initData)
      return false;

    decoding_decrypter_ = static_cast<WV_CencSingleSampleDecrypter*>(decrypter);
    return decoding_decrypter_->OpenVideoDecoder(initData);
  }

  virtual SSD_DECODE_RETVAL DecodeVideo(void* hostInstance, SSD_SAMPLE *sample, SSD_PICTURE *picture) override
  {
    if (!decoding_decrypter_)
      return VC_ERROR;

    return decoding_decrypter_->DecodeVideo(hostInstance, sample, picture);
  }

  virtual void ResetVideo() override
  {
    if (decoding_decrypter_)
      decoding_decrypter_->ResetVideo();
  }

private:
  WV_DRM *cdmsession_;
  WV_CencSingleSampleDecrypter *decoding_decrypter_;
};

extern "C" {

#ifdef _WIN32
#define MODULE_API __declspec(dllexport)
#else
#define MODULE_API
#endif

  SSD_DECRYPTER MODULE_API *CreateDecryptorInstance(class SSD_HOST *h, uint32_t host_version)
  {
    if (host_version != SSD_HOST::version)
      return 0;
    host = h;
    return new WVDecrypter();
  };

  void MODULE_API DeleteDecryptorInstance(SSD_DECRYPTER *d)
  {
    delete static_cast<WVDecrypter*>(d);
  }
};
