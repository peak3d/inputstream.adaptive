// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cdm_adapter.h"
#include <chrono>
#include <thread>

#define DCHECK(condition) assert(condition)

#include "../base/limits.h"

#ifdef __APPLE__
#include <sys/time.h>
//clock_gettime is not implemented on OSX
int clock_gettime(int clk_id, struct timespec* t) {
  struct timeval now;
  int rv = gettimeofday(&now, NULL);
  if (rv) return rv;
  t->tv_sec  = now.tv_sec;
  t->tv_nsec = now.tv_usec * 1000;
  return 0;
}
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif
#endif

namespace media {

uint64_t gtc()
{
#ifdef OS_WIN
  return GetTickCount64();
#else
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  return  tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
#endif
}

namespace {

void* GetCdmHost(int host_interface_version, void* user_data)
{
  if (!user_data)
    return nullptr;

  CdmAdapter *adapter = static_cast<CdmAdapter*>(user_data);

  switch (host_interface_version)
  {
    case cdm::Host_9::kVersion:
      return static_cast<cdm::Host_9*>(adapter);
    case cdm::Host_10::kVersion:
      return static_cast<cdm::Host_10*>(adapter);
    case cdm::Host_11::kVersion:
      return static_cast<cdm::Host_11*>(adapter);
    default:
      return nullptr;
  }
}

}  // namespace

std::atomic<bool> exit_thread_flag;
std::atomic<bool> timer_thread_running;

void timerfunc(std::shared_ptr<CdmAdapter> adp, uint64_t delay, void* context)
{
  timer_thread_running  = true;
  uint64_t waited = 0;
  while (!exit_thread_flag && delay > waited) 
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    waited += 100;
  }
  if (!exit_thread_flag) 
  {
    adp->TimerExpired(context);
  }
  timer_thread_running = false;
}

cdm::AudioDecoderConfig_1 ToAudioDecoderConfig_1(
  const cdm::AudioDecoderConfig_2& config) {
  return{ config.codec,
    config.channel_count,
    config.bits_per_channel,
    config.samples_per_second,
    config.extra_data,
    config.extra_data_size };
}

cdm::VideoDecoderConfig_1 ToVideoDecoderConfig_1(
  const cdm::VideoDecoderConfig_3& config) {
  return{ config.codec,      config.profile,    config.format,
    config.coded_size, config.extra_data, config.extra_data_size };
}

cdm::VideoDecoderConfig_2 ToVideoDecoderConfig_2(
  const cdm::VideoDecoderConfig_3& config) {
  return{ config.codec,
    config.profile,
    config.format,
    config.coded_size,
    config.extra_data,
    config.extra_data_size,
    config.encryption_scheme };
}

cdm::InputBuffer_1 ToInputBuffer_1(const cdm::InputBuffer_2& buffer) {
  return{ buffer.data,       buffer.data_size,
    buffer.key_id,     buffer.key_id_size,
    buffer.iv,         buffer.iv_size,
    buffer.subsamples, buffer.num_subsamples,
    buffer.timestamp };
}

/*******************************         CdmAdapter        ****************************************/


CdmAdapter::CdmAdapter(
  const std::string& key_system,
  const std::string& cdm_path,
  const std::string& base_path,
  const CdmConfig& cdm_config,
  CdmAdapterClient *client)
: library_(0)
, cdm_path_(cdm_path)
, cdm_base_path_(base_path)
, client_(client)
, key_system_(key_system)
, cdm_config_(cdm_config)
, active_buffer_(0)
, cdm9_(0), cdm10_(0), cdm11_(0)
, session_active_(false)
{
  //DCHECK(!key_system_.empty());
  Initialize();
}

CdmAdapter::~CdmAdapter()
{
  exit_thread_flag = true;
  while (timer_thread_running) 
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (cdm9_)
    cdm9_->Destroy(), cdm9_ = nullptr;
  else if (cdm10_)
    cdm10_->Destroy(), cdm10_ = nullptr;
  else if (cdm11_)
    cdm11_->Destroy(), cdm11_ = nullptr;
  else
    return;

  deinit_cdm_func();

  base::UnloadNativeLibrary(library_);
}

void CdmAdapter::Initialize()
{
  exit_thread_flag = false;
  timer_thread_running = false;
  if (cdm9_ || cdm10_ || cdm11_)
  {
    if (cdm9_)
      cdm9_->Destroy(), cdm9_ = nullptr;
    else if (cdm10_)
      cdm10_->Destroy(), cdm10_ = nullptr;
    else if (cdm11_)
      cdm11_->Destroy(), cdm11_ = nullptr;
    base::UnloadNativeLibrary(library_);
    library_ = 0;
  }

  base::NativeLibraryLoadError error;
  library_ = base::LoadNativeLibrary(cdm_path_, &error);

  if (!library_)
  {
    std::string log_error = "CDM LoadNativeLibrary error: " + error.ToString();
    client_->CDMLog(log_error.c_str());
    return;
  }

  init_cdm_func = reinterpret_cast<InitializeCdmModuleFunc>(base::GetFunctionPointerFromNativeLibrary(library_, MAKE_STRING(INITIALIZE_CDM_MODULE)));
  deinit_cdm_func = reinterpret_cast<DeinitializeCdmModuleFunc>(base::GetFunctionPointerFromNativeLibrary(library_, "DeinitializeCdmModule"));
  create_cdm_func = reinterpret_cast<CreateCdmFunc>(base::GetFunctionPointerFromNativeLibrary(library_, "CreateCdmInstance"));
  get_cdm_verion_func = reinterpret_cast<GetCdmVersionFunc>(base::GetFunctionPointerFromNativeLibrary(library_, "GetCdmVersion"));

  if (!init_cdm_func || !create_cdm_func || !get_cdm_verion_func || !deinit_cdm_func)
  {
    base::UnloadNativeLibrary(library_);
    library_ = 0;
    return;
  }

  std::string version = get_cdm_verion_func();
  version = "CDM version: " + version;
  client_->CDMLog(version.c_str());

#if defined(OS_WIN)
  // Load DXVA before sandbox lockdown to give CDM access to Output Protection
  // Manager (OPM).
  base::LoadNativeLibrary("dxva2.dll", &error);
#endif  // defined(OS_WIN)

  init_cdm_func();

  cdm11_ = static_cast<cdm::ContentDecryptionModule_11*>(create_cdm_func(11, key_system_.data(), key_system_.size(), GetCdmHost, this));

  if (!cdm11_)
  {
    cdm10_ = static_cast<cdm::ContentDecryptionModule_10*>(create_cdm_func(10, key_system_.data(), key_system_.size(), GetCdmHost, this));

    if (!cdm10_)
      cdm9_ = reinterpret_cast<cdm::ContentDecryptionModule_9*>(create_cdm_func(9, key_system_.data(), key_system_.size(), GetCdmHost, this));
  }

  if (cdm9_ || cdm10_ || cdm11_)
  {
    if (cdm9_)
      cdm9_->Initialize(cdm_config_.allow_distinctive_identifier,
        cdm_config_.allow_persistent_state);
    else if(cdm10_)
      cdm10_->Initialize(cdm_config_.allow_distinctive_identifier,
        cdm_config_.allow_persistent_state, false);
    else if (cdm11_)
      cdm11_->Initialize(cdm_config_.allow_distinctive_identifier,
        cdm_config_.allow_persistent_state, false);
  }
  else
  {
    base::UnloadNativeLibrary(library_);
    library_ = 0;
  }
}

void CdmAdapter::SendClientMessage(const char* session, uint32_t session_size, CdmAdapterClient::CDMADPMSG msg, const uint8_t *data, size_t data_size, uint32_t status)
{
  std::lock_guard<std::mutex> guard(client_mutex_);
  if (client_)
    client_->OnCDMMessage(session, session_size, msg, data, data_size, status);
}

void CdmAdapter::RemoveClient()
{
  std::lock_guard<std::mutex> guard(client_mutex_);
  client_ = nullptr;
}

void CdmAdapter::SetServerCertificate(uint32_t promise_id,
  const uint8_t* server_certificate_data,
  uint32_t server_certificate_data_size)
{
  if (server_certificate_data_size < limits::kMinCertificateLength ||
    server_certificate_data_size > limits::kMaxCertificateLength) {
  return;
  }
  if (cdm9_)
    cdm9_->SetServerCertificate(promise_id, server_certificate_data,
      server_certificate_data_size);
  else if (cdm10_)
    cdm10_->SetServerCertificate(promise_id, server_certificate_data,
      server_certificate_data_size);
  else if (cdm11_)
    cdm11_->SetServerCertificate(promise_id, server_certificate_data,
      server_certificate_data_size);
}

void CdmAdapter::CreateSessionAndGenerateRequest(uint32_t promise_id,
  cdm::SessionType session_type,
  cdm::InitDataType init_data_type,
  const uint8_t* init_data,
  uint32_t init_data_size)
{
  if (cdm9_)
    cdm9_->CreateSessionAndGenerateRequest(
      promise_id, session_type,
      init_data_type, init_data,
      init_data_size);
  else  if (cdm10_)
    cdm10_->CreateSessionAndGenerateRequest(
      promise_id, session_type,
      init_data_type, init_data,
      init_data_size);
  else  if (cdm11_)
    cdm11_->CreateSessionAndGenerateRequest(
      promise_id, session_type,
      init_data_type, init_data,
      init_data_size);
}

void CdmAdapter::LoadSession(uint32_t promise_id,
  cdm::SessionType session_type,
  const char* session_id,
  uint32_t session_id_size)
{
  if (cdm9_)
    cdm9_->LoadSession(promise_id, session_type,
      session_id, session_id_size);
  else if (cdm10_)
    cdm10_->LoadSession(promise_id, session_type,
      session_id, session_id_size);
  else if (cdm11_)
    cdm11_->LoadSession(promise_id, session_type,
      session_id, session_id_size);
}

void CdmAdapter::UpdateSession(uint32_t promise_id,
  const char* session_id,
  uint32_t session_id_size,
  const uint8_t* response,
  uint32_t response_size)
{
  if (cdm9_)
    cdm9_->UpdateSession(promise_id, session_id, session_id_size,
            response, response_size);
  else if(cdm10_)
    cdm10_->UpdateSession(promise_id, session_id, session_id_size,
            response, response_size);
  else if (cdm11_)
    cdm11_->UpdateSession(promise_id, session_id, session_id_size,
      response, response_size);
}

void CdmAdapter::SetSessionActive(bool isActive)
{
  session_active_ = isActive;
}

bool CdmAdapter::IsSessionActive()
{
  return session_active_;
}

void CdmAdapter::CloseSession(uint32_t promise_id,
  const char* session_id,
  uint32_t session_id_size)
{
  session_active_ = false;
  exit_thread_flag = true;
  while (timer_thread_running) 
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (cdm9_)
    cdm9_->CloseSession(promise_id, session_id, session_id_size);
  else if (cdm10_)
    cdm10_->CloseSession(promise_id, session_id, session_id_size);
  else if (cdm11_)
    cdm11_->CloseSession(promise_id, session_id, session_id_size);
}

void CdmAdapter::RemoveSession(uint32_t promise_id,
  const char* session_id,
  uint32_t session_id_size)
{
  if (cdm9_)
    cdm9_->RemoveSession(promise_id, session_id, session_id_size);
  else if (cdm10_)
    cdm10_->RemoveSession(promise_id, session_id, session_id_size);
  else if (cdm11_)
    cdm11_->RemoveSession(promise_id, session_id, session_id_size);
}

void CdmAdapter::TimerExpired(void* context)
{
  if (cdm9_)
    cdm9_->TimerExpired(context);
  else if (cdm10_)
    cdm10_->TimerExpired(context);
  else if (cdm11_)
    cdm11_->TimerExpired(context);
}

cdm::Status CdmAdapter::Decrypt(const cdm::InputBuffer_2& encrypted_buffer,
  cdm::DecryptedBlock* decrypted_buffer)
{
  //We need this wait here for fast systems, during buffering
  //widewine stopps if some seconds (5??) are fetched too fast
  //std::this_thread::sleep_for(std::chrono::milliseconds(5));

  std::lock_guard<std::mutex> lock(decrypt_mutex_);

  active_buffer_ = decrypted_buffer->DecryptedBuffer();
  cdm::Status ret;

  if (cdm9_)
    ret = cdm9_->Decrypt(ToInputBuffer_1(encrypted_buffer), decrypted_buffer);
  else if (cdm10_)
    ret = cdm10_->Decrypt(encrypted_buffer, decrypted_buffer);
  else if (cdm11_)
  {
    cdm::InputBuffer_2 tmp(encrypted_buffer);
    ret = cdm11_->Decrypt(tmp, decrypted_buffer);
  }

  active_buffer_ = 0;
  return ret;
}

cdm::Status CdmAdapter::InitializeAudioDecoder(
  const cdm::AudioDecoderConfig_2& audio_decoder_config)
{
  if (cdm9_)
    return cdm9_->InitializeAudioDecoder(ToAudioDecoderConfig_1(audio_decoder_config));
  else if (cdm10_)
    return cdm10_->InitializeAudioDecoder(audio_decoder_config);
  else if (cdm11_)
    return cdm11_->InitializeAudioDecoder(audio_decoder_config);
  return cdm::kDeferredInitialization;
}

cdm::Status CdmAdapter::InitializeVideoDecoder(
  const cdm::VideoDecoderConfig_3& video_decoder_config)
{
  if (cdm9_)
    return cdm9_->InitializeVideoDecoder(ToVideoDecoderConfig_1(video_decoder_config));
  else if (cdm10_)
    return cdm10_->InitializeVideoDecoder(ToVideoDecoderConfig_2(video_decoder_config));
  else if (cdm11_)
    return cdm11_->InitializeVideoDecoder(video_decoder_config);
  return cdm::kDeferredInitialization;
}

void CdmAdapter::DeinitializeDecoder(cdm::StreamType decoder_type)
{
  if (cdm9_)
    cdm9_->DeinitializeDecoder(decoder_type);
  else if (cdm10_)
    cdm10_->DeinitializeDecoder(decoder_type);
  else if (cdm11_)
    cdm11_->DeinitializeDecoder(decoder_type);
}

void CdmAdapter::ResetDecoder(cdm::StreamType decoder_type)
{
  if (cdm9_)
    cdm9_->ResetDecoder(decoder_type);
  else if (cdm10_)
    cdm10_->ResetDecoder(decoder_type);
  else if (cdm11_)
    cdm11_->ResetDecoder(decoder_type);
}

cdm::Status CdmAdapter::DecryptAndDecodeFrame(const cdm::InputBuffer_2& encrypted_buffer,
  CdmVideoFrame* video_frame)
{
  std::lock_guard<std::mutex> lock(decrypt_mutex_);
  cdm::Status ret(cdm::kDeferredInitialization);

  if (cdm9_)
    ret = cdm9_->DecryptAndDecodeFrame(ToInputBuffer_1(encrypted_buffer), video_frame);
  else if (cdm10_)
    ret = cdm10_->DecryptAndDecodeFrame(encrypted_buffer, video_frame);
  else if (cdm11_)
    ret = cdm11_->DecryptAndDecodeFrame(encrypted_buffer, video_frame);

  active_buffer_ = 0;
  return ret;
}

cdm::Status CdmAdapter::DecryptAndDecodeSamples(const cdm::InputBuffer_2& encrypted_buffer,
  cdm::AudioFrames* audio_frames)
{
  std::lock_guard<std::mutex> lock(decrypt_mutex_);
  if (cdm9_)
    return cdm9_->DecryptAndDecodeSamples(ToInputBuffer_1(encrypted_buffer), audio_frames);
  else if (cdm10_)
    return cdm10_->DecryptAndDecodeSamples(encrypted_buffer, audio_frames);
  else if (cdm11_)
    return cdm11_->DecryptAndDecodeSamples(encrypted_buffer, audio_frames);
  return cdm::kDeferredInitialization;
}

void CdmAdapter::OnPlatformChallengeResponse(
  const cdm::PlatformChallengeResponse& response)
{
  if (cdm9_)
    cdm9_->OnPlatformChallengeResponse(response);
  else if (cdm10_)
    cdm10_->OnPlatformChallengeResponse(response);
  else if (cdm11_)
    cdm11_->OnPlatformChallengeResponse(response);
}

void CdmAdapter::OnQueryOutputProtectionStatus(cdm::QueryResult result,
  uint32_t link_mask,
  uint32_t output_protection_mask)
{
  if (cdm9_)
    cdm9_->OnQueryOutputProtectionStatus(result, link_mask,
      output_protection_mask);
  else if (cdm10_)
    cdm10_->OnQueryOutputProtectionStatus(result, link_mask,
      output_protection_mask);
  else if (cdm11_)
    cdm11_->OnQueryOutputProtectionStatus(result, link_mask,
      output_protection_mask);
}

/******************************** HOST *****************************************/

cdm::Buffer* CdmAdapter::Allocate(uint32_t capacity)
{
  if (active_buffer_)
  return active_buffer_;
  else
  return client_->AllocateBuffer(capacity);
}

void CdmAdapter::SetTimer(int64_t delay_ms, void* context)
{
  //LICENSERENEWAL
  if (session_active_)
  {
    exit_thread_flag = false;
    std::thread(timerfunc, shared_from_this(), delay_ms, context).detach();
  }
}

cdm::Time CdmAdapter::GetCurrentWallTime()
{
  cdm::Time res = static_cast<cdm::Time>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  return res / 1000.0;
}

void CdmAdapter::OnResolvePromise(uint32_t promise_id)
{
}

void CdmAdapter::OnResolveNewSessionPromise(uint32_t promise_id,
                      const char* session_id,
                      uint32_t session_id_size)
{
}

void CdmAdapter::OnSessionKeysChange(const char* session_id,
                                  uint32_t session_id_size,
                                  bool has_additional_usable_key,
                                  const cdm::KeyInformation* keys_info,
                                  uint32_t keys_info_count)
{
  for (uint32_t i(0); i < keys_info_count; ++i)
  {
    char fmtbuf[128], *fmtptr(fmtbuf+11);
    strcpy(fmtbuf, "Sessionkey: ");
    for (unsigned int j(0); j < keys_info[i].key_id_size; ++j)
      fmtptr += sprintf(fmtptr, "%02X", (int)keys_info[i].key_id[j]);
    sprintf(fmtptr, " status: %d syscode: %u", keys_info[i].status, keys_info[i].system_code);
    client_->CDMLog(fmtbuf);

    SendClientMessage(session_id, session_id_size, CdmAdapterClient::kSessionKeysChange,
      keys_info[i].key_id, keys_info[i].key_id_size, keys_info[i].status);
  }
}

void CdmAdapter::OnExpirationChange(const char* session_id,
                  uint32_t session_id_size,
                  cdm::Time new_expiry_time)
{
  SendClientMessage(session_id, session_id_size, CdmAdapterClient::kSessionExpired, nullptr, 0, 0);
}

void CdmAdapter::OnSessionClosed(const char* session_id,
                 uint32_t session_id_size)
{
  SendClientMessage(session_id, session_id_size, CdmAdapterClient::kSessionClosed, nullptr, 0, 0);
}

void CdmAdapter::SendPlatformChallenge(const char* service_id,
                                    uint32_t service_id_size,
                                    const char* challenge,
                                    uint32_t challenge_size)
{
}

void CdmAdapter::EnableOutputProtection(uint32_t desired_protection_mask)
{
  QueryOutputProtectionStatus();
}

void CdmAdapter::QueryOutputProtectionStatus()
{
  OnQueryOutputProtectionStatus(cdm::kQuerySucceeded, cdm::kLinkTypeInternal, cdm::kProtectionHDCP);
}

void CdmAdapter::OnDeferredInitializationDone(cdm::StreamType stream_type,
                        cdm::Status decoder_status)
{
}

// The CDM owns the returned object and must call FileIO::Close() to release it.
cdm::FileIO* CdmAdapter::CreateFileIO(cdm::FileIOClient* client)
{
  return new CdmFileIoImpl(cdm_base_path_, client);
}


// Host_9 specific implementations
void CdmAdapter::OnResolveKeyStatusPromise(uint32_t promise_id, cdm::KeyStatus key_status)
{
}

void CdmAdapter::OnRejectPromise(uint32_t promise_id, cdm::Exception exception,
  uint32_t system_code, const char* error_message, uint32_t error_message_size)
{
}

void CdmAdapter::OnSessionMessage(const char* session_id, uint32_t session_id_size,
  cdm::MessageType message_type, const char* message, uint32_t message_size)
{
  SendClientMessage(session_id, session_id_size, CdmAdapterClient::kSessionMessage, reinterpret_cast<const uint8_t*>(message), message_size, 0);
}

void CdmAdapter::RequestStorageId(uint32_t version)
{
  if (cdm10_)
    cdm10_->OnStorageId(1, nullptr, 0);
  else if (cdm11_)
    cdm11_->OnStorageId(1, nullptr, 0);
}

void CdmAdapter::OnInitialized(bool success)
{
  char fmtbuf[64];
  sprintf(fmtbuf, "cdm::OnInitialized: %s", success ? "true" : "false");
  client_->CDMLog(fmtbuf);
}


/*******************************         CdmFileIoImpl        ****************************************/

CdmFileIoImpl::CdmFileIoImpl(std::string base_path, cdm::FileIOClient* client)
  : base_path_(base_path)
  , client_(client)
  , file_descriptor_(0)
  , data_buffer_(0)
  , opened_(false)
{
}

void CdmFileIoImpl::Open(const char* file_name, uint32_t file_name_size)
{
  if (!opened_)
  {
  opened_ = true;
  base_path_ += std::string(file_name, file_name_size);
  client_->OnOpenComplete(cdm::FileIOClient::Status::kSuccess);
  }
  else
  client_->OnOpenComplete(cdm::FileIOClient::Status::kInUse);
}

void CdmFileIoImpl::Read()
{
  cdm::FileIOClient::Status status(cdm::FileIOClient::Status::kError);
  size_t sz(0);

  free(reinterpret_cast<void*>(data_buffer_));
  data_buffer_ = nullptr;

  file_descriptor_ = fopen(base_path_.c_str(), "rb");

  if (file_descriptor_)
  {
    status = cdm::FileIOClient::Status::kSuccess;
    fseek(file_descriptor_, 0, SEEK_END);
    sz = ftell(file_descriptor_);
    if (sz)
    {
      fseek(file_descriptor_, 0, SEEK_SET);
      if ((data_buffer_ = reinterpret_cast<uint8_t*>(malloc(sz))) == nullptr || fread(data_buffer_, 1, sz, file_descriptor_) != sz)
      status = cdm::FileIOClient::Status::kError;
    }
  } else
    status = cdm::FileIOClient::Status::kSuccess;
  client_->OnReadComplete(status, data_buffer_, sz);
}

void CdmFileIoImpl::Write(const uint8_t* data, uint32_t data_size)
{
  cdm::FileIOClient::Status status(cdm::FileIOClient::Status::kError);
  file_descriptor_ = fopen(base_path_.c_str(), "wb");

  if (file_descriptor_)
  {
    if (fwrite(data, 1, data_size, file_descriptor_) == data_size)
      status = cdm::FileIOClient::Status::kSuccess;
  }
  client_->OnWriteComplete(status);
}

void CdmFileIoImpl::Close()
{
  if (file_descriptor_)
  {
    fclose(file_descriptor_);
    file_descriptor_ = 0;
  }
  client_ = 0;
  free(reinterpret_cast<void*>(data_buffer_));
  data_buffer_ = 0;
  delete this;
}

}  // namespace media
