#include "../Iaes_decrypter.h"
#include "../log.h"
#include "../common/AdaptiveStream.h"
#include "../parser/DASHTree.h"
#include "../parser/HLSTree.h"

std::string GetEnv(const std::string& var);
void SetFileName(std::string& file, const std::string name);
void Log(const LogLevel loglevel, const char* format, ...);

class testHelper
{
public:
  static std::string testFile;
  static std::string effectiveUrl;
  static std::string lastDownloadUrl;
};

class TestAdaptiveStream : public adaptive::AdaptiveStream
{
public:
  TestAdaptiveStream(adaptive::AdaptiveTree& tree, adaptive::AdaptiveTree::StreamType type)
    : adaptive::AdaptiveStream(tree, type){};
  std::chrono::system_clock::time_point mock_time_stream = std::chrono::system_clock::now();
  void SetLastUpdated(std::chrono::system_clock::time_point tm) override { lastUpdated_ = tm; };

protected:
  virtual bool download(const char* url,
                        const std::map<std::string, std::string>& mediaHeaders) override;
};

class AESDecrypter : public IAESDecrypter
{
public:
  AESDecrypter(const std::string& licenseKey) : m_licenseKey(licenseKey){};
  virtual ~AESDecrypter() = default;

  void decrypt(const AP4_UI08* aes_key,
               const AP4_UI08* aes_iv,
               const AP4_UI08* src,
               AP4_UI08* dst,
               size_t dataSize);
  std::string convertIV(const std::string& input);
  void ivFromSequence(uint8_t* buffer, uint64_t sid);
  const std::string& getLicenseKey() const { return m_licenseKey; };
  bool RenewLicense(const std::string& pluginUrl);

private:
  std::string m_licenseKey;
};

class DASHTestTree : public adaptive::DASHTree
{
public:
  uint64_t mock_time = 10000000L;
  std::chrono::system_clock::time_point mock_time_chrono = std::chrono::system_clock::now();
  DASHTestTree();
  uint64_t GetNowTime() override { return mock_time; }
  std::chrono::system_clock::time_point GetNowTimeChrono() { return mock_time_chrono; };
  void SetLastUpdated(std::chrono::system_clock::time_point tm) override { lastUpdated_ = tm; };
};
