#pragma once

//Functionality wich is supported by the Decrypter
class AP4_CencSingleSampleDecrypter;
class AP4_DataBuffer;

namespace SSD
{
  struct SSD_PICTURE;

  //Functionality wich is supported by the Addon
  class SSD_HOST
  {
  public:
    enum CURLOPTIONS
    {
      OPTION_PROTOCOL,
      OPTION_HEADER
    };
    enum CURLPROPERTY
    {
      PROPERTY_HEADER
  };
    static const uint32_t version = 13;
#if defined(ANDROID)
    virtual void* GetJNIEnv() = 0;
    virtual int GetSDKVersion() = 0;
    virtual const char *GetClassName() = 0;
#endif
    virtual const char *GetLibraryPath() const = 0;
    virtual const char *GetProfilePath() const = 0;
    virtual void* CURLCreate(const char* strURL) = 0;
    virtual bool CURLAddOption(void* file, CURLOPTIONS opt, const char* name, const char* value) = 0;
    virtual const char* CURLGetProperty(void* file, CURLPROPERTY prop, const char *name) = 0;
    virtual bool CURLOpen(void* file) = 0;
    virtual size_t ReadFile(void* file, void* lpBuf, size_t uiBufSize) = 0;
    virtual void CloseFile(void* file) = 0;
    virtual bool CreateDir(const char* dir) = 0;
    virtual bool GetBuffer(void* instance, SSD_PICTURE &picture) = 0;
    virtual void ReleaseBuffer(void* instance, void *buffer) = 0;

    enum LOGLEVEL
    {
      LL_DEBUG,
      LL_INFO,
      LL_ERROR
    };

    virtual void Log(LOGLEVEL level, const char *msg) = 0;
  };

  /****************************************************************************************************/
  // keep those values in track with xbmc\addons\kodi-addon-dev-kit\include\kodi\kodi_videocodec_types.h
  /****************************************************************************************************/

  enum SSD_VIDEOFORMAT
  {
    UnknownVideoFormat = 0,
    VideoFormatYV12,
    VideoFormatI420,
    MaxVideoFormats
  };

  struct SSD_VIDEOINITDATA
  {
    enum Codec {
      CodecUnknown = 0,
      CodecVp8,
      CodecH264,
      CodecVp9
    } codec;

    enum CodecProfile
    {
      CodecProfileUnknown = 0,
      CodecProfileNotNeeded,
      H264CodecProfileBaseline,
      H264CodecProfileMain,
      H264CodecProfileExtended,
      H264CodecProfileHigh,
      H264CodecProfileHigh10,
      H264CodecProfileHigh422,
      H264CodecProfileHigh444Predictive
    } codecProfile;

    const SSD_VIDEOFORMAT *videoFormats;

    uint32_t width, height;

    const uint8_t *extraData;
    unsigned int extraDataSize;
  };

  struct SSD_PICTURE
  {
    enum VideoPlane {
      YPlane = 0,
      UPlane,
      VPlane,
      MaxPlanes = 3,
    };

    enum Flags : uint32_t
    {
      FLAG_NONE = 0,
      FLAG_DROP = (1 << 0),
      FLAG_DRAIN = (1 << 1),
    };

    SSD_VIDEOFORMAT videoFormat;
    uint32_t flags;

    uint32_t width, height;

    uint8_t *decodedData;
    size_t decodedDataSize;

    uint32_t planeOffsets[VideoPlane::MaxPlanes];
    uint32_t stride[VideoPlane::MaxPlanes];

    int64_t pts;

    void *buffer;
  };

  typedef struct SSD_SAMPLE
  {
    const uint8_t *data;
    uint32_t dataSize;

    int64_t pts;

    uint16_t numSubSamples; //number of subsamples
    uint16_t flags; //flags for later use

    uint16_t *clearBytes; // numSubSamples uint16_t's wich define the size of clear size of a subsample
    uint32_t *cipherBytes; // numSubSamples uint32_t's wich define the size of cipher size of a subsample

    uint8_t *iv;  // initialization vector
    uint8_t *kid; // key id
  } SSD_SAMPLE;

  enum SSD_DECODE_RETVAL
  {
    VC_NONE = 0,        //< noop
    VC_ERROR,           //< an error occured, no other messages will be returned
    VC_BUFFER,          //< the decoder needs more data
    VC_PICTURE,         //< the decoder got a picture
    VC_EOF,             //< the decoder signals EOF
  };

  class SSD_DECRYPTER
  {
  public:
    struct SSD_CAPS
    {
      static const uint32_t SSD_SUPPORTS_DECODING = 1;
      static const uint32_t SSD_SECURE_PATH = 2;
      static const uint32_t SSD_ANNEXB_REQUIRED = 4;
      static const uint32_t SSD_HDCP_RESTRICTED = 8;
      static const uint32_t SSD_SINGLE_DECRYPT = 16;
      static const uint32_t SSD_SECURE_DECODER = 32;
      static const uint32_t SSD_INVALID = 64;

      static const uint32_t SSD_MEDIA_VIDEO = 1;
      static const uint32_t SSD_MEDIA_AUDIO = 2;

      uint16_t flags;

      /* The following 2 fields are set as followed:
      - If licenseresponse return hdcp information, hdcpversion is 0 and
      hdcplimit either 0 (if hdcp is supported) or given value (if hdcpversion is not supported)
      - if no hdcp information is passed in licenseresponse, we set hdcpversion to the value we support
      manifest / representation have to check if they are allowed to be played.
      */
      uint16_t hdcpVersion; //The HDCP version streams has to be restricted 0,10,20,21,22.....
      uint32_t hdcpLimit; // If set, streams wich wxh > this value cannot be played.
    };

    static const uint8_t CONFIG_PERSISTENTSTORAGE = 1;

    // Return supported URN if type matches to capabilities, otherwise null
    virtual const char *SelectKeySytem(const char* keySystem) = 0;
    virtual bool OpenDRMSystem(const char *licenseURL, const AP4_DataBuffer &serverCertificate, const uint8_t config) = 0;
    virtual AP4_CencSingleSampleDecrypter *CreateSingleSampleDecrypter(AP4_DataBuffer &pssh, const char *optionalKeyParameter, const uint8_t *defaultkeyid, bool skipSessionMessage) = 0;
    virtual void DestroySingleSampleDecrypter(AP4_CencSingleSampleDecrypter* decrypter) = 0;

    virtual void GetCapabilities(AP4_CencSingleSampleDecrypter* decrypter, const uint8_t *keyid, uint32_t media, SSD_DECRYPTER::SSD_CAPS &caps) = 0;
    virtual bool HasLicenseKey(AP4_CencSingleSampleDecrypter* decrypter, const uint8_t *keyid) = 0;
    virtual bool HasCdmSession() = 0;
    virtual std::string GetChallengeB64Data(AP4_CencSingleSampleDecrypter* decrypter) = 0;

    virtual bool OpenVideoDecoder(AP4_CencSingleSampleDecrypter* decrypter, const SSD_VIDEOINITDATA *initData) = 0;
    virtual SSD_DECODE_RETVAL DecodeVideo(void* instance, SSD_SAMPLE *sample, SSD_PICTURE *picture) = 0;
    virtual void ResetVideo() = 0;
  };
}; // namespace
