#include "TestHelper.h"

#include <gtest/gtest.h>


class DASHTreeTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    tree = new DASHTestTree;
    tree->supportedKeySystem_ = "urn:uuid:EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED";
    tree->videoStream = new TestAdaptiveStream(*tree, adaptive::AdaptiveTree::StreamType::VIDEO);
  }

  void TearDown() override
  {
    testHelper::effectiveUrl.clear();
    testHelper::lastDownloadUrl.clear();
    delete tree;
    tree = nullptr;
  }

  void OpenTestFile(std::string testfilename, std::string url, std::string manifestHeaders)
  {
    SetFileName(testHelper::testFile, testfilename);
    if (!tree->open(url, manifestHeaders))
    {
      printf("open() failed");
      exit(1);
    }
  }

  void ReadSegments(TestAdaptiveStream* stream,
                    uint32_t reads,
                    bool clearUrls = true)
  {
    // Rudimentary simulation of running a stream and consuming segment data.
    // Normally AdaptiveStream::read is called from a sample reader for the exact
    // amount of bytes needed to supply the next sample until the segment is
    // exhausted. Here our segments are a fixed size (16 bytes) and for testing we can
    // optimally call to read 1 segment per AdaptiveStream::read

    if (clearUrls)
      tree->downloadedUrls.clear();

    for (unsigned int i = 0; i < reads; i++)
      if (stream->read(buf, sizeof(buf)))
        tree->downloadedUrls.push_back(testHelper::lastDownloadUrl);
      else
        break;
  }

  unsigned char buf[16];
  DASHTestTree* tree;
};

TEST_F(DASHTreeTest, CalculateBaseURL)
{
  // No BaseURL tags
  OpenTestFile("mpd/segtpl.mpd", "https://foo.bar/mpd/test.mpd", "");
  EXPECT_EQ(tree->base_url_, "https://foo.bar/mpd/");
}

TEST_F(DASHTreeTest, CalculateBaseDomain)
{
  OpenTestFile("mpd/segtpl.mpd", "https://foo.bar/mpd/test.mpd", "");

  EXPECT_EQ(tree->base_domain_, "https://foo.bar");
}

TEST_F(DASHTreeTest, CalculateEffectiveUrlFromRedirect)
{
  // like base_url_, effective_url_ should be path, not including filename
  testHelper::effectiveUrl = "https://foo.bar/mpd/stream.mpd";
  OpenTestFile("mpd/segtpl.mpd", "https://bit.ly/abcd", "");

  EXPECT_EQ(tree->effective_url_, "https://foo.bar/mpd/");
}

TEST_F(DASHTreeTest, CalculateBaseURLFromBaseURLTag)
{
  OpenTestFile("mpd/segtpl_baseurlinmpd.mpd", "https://bit.ly/abcd", "");
  EXPECT_EQ(tree->current_period_->base_url_, "https://foo.bar/mpd/");
}

TEST_F(DASHTreeTest, CalculateSegTplWithNoSlashs)
{
  // BaseURL inside period with no trailing slash, uses segtpl, media/init doesn't start with slash
  OpenTestFile("mpd/segtpl_baseurl_noslashs.mpd", "https://foo.bar/initialpath/test.mpd", "");

  adaptive::AdaptiveTree::SegmentTemplate segtpl =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segtpl_;

  EXPECT_EQ(segtpl.initialization, "https://foo.bar/guid.ism/dash/media-video=66000.dash");
  EXPECT_EQ(segtpl.media, "https://foo.bar/guid.ism/dash/media-video=66000-$Number$.m4s");
}

TEST_F(DASHTreeTest, CalculateSegTplWithMediaInitSlash)
{
  // BaseURL inside period with no trailing slash, uses segtpl, media/init starts with slash
  OpenTestFile("mpd/segtpl_slash_baseurl_noslash.mpd", "https://foo.bar/initialpath/test.mpd", "");

  adaptive::AdaptiveTree::SegmentTemplate segtpl =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segtpl_;

  EXPECT_EQ(segtpl.initialization, "https://foo.bar/media-video=66000.dash");
  EXPECT_EQ(segtpl.media, "https://foo.bar/media-video=66000-$Number$.m4s");
}

TEST_F(DASHTreeTest, CalculateSegTplWithBaseURLSlash)
{
  // BaseURL inside period with trailing slash, uses segtpl, media/init doesn't start with slash
  OpenTestFile("mpd/segtpl_noslash_baseurl_slash.mpd", "https://foo.bar/initialpath/test.mpd", "");

  adaptive::AdaptiveTree::SegmentTemplate segtpl =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segtpl_;

  EXPECT_EQ(segtpl.initialization, "https://foo.bar/guid.ism/dash/media-video=66000.dash");
  EXPECT_EQ(segtpl.media, "https://foo.bar/guid.ism/dash/media-video=66000-$Number$.m4s");
}

TEST_F(DASHTreeTest, CalculateSegTplWithBaseURLAndMediaInitSlash)
{
  // BaseURL inside period with trailing slash, uses segtpl, media/init starts with slash
  OpenTestFile("mpd/segtpl_slash_baseurl_slash.mpd", "https://foo.bar/initialpath/test.mpd", "");

  adaptive::AdaptiveTree::SegmentTemplate segtpl =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segtpl_;

  EXPECT_EQ(segtpl.initialization, "https://foo.bar/media-video=66000.dash");
  EXPECT_EQ(segtpl.media, "https://foo.bar/media-video=66000-$Number$.m4s");
}

TEST_F(DASHTreeTest, CalculateBaseURLInRepRangeBytes)
{
  // Byteranged indexing
  OpenTestFile("mpd/segmentbase.mpd", "https://foo.bar/test.mpd", "");
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[0]->representations_[0]->url_,
            "https://foo.bar/video/23.98p/r0/vid10.mp4");
}

TEST_F(DASHTreeTest, CalculateCorrectSegmentNumbersFromSegmentTimeline)
{
  // SegmentTimeline, availabilityStartTime is greater than epoch
  OpenTestFile("mpd/segtimeline_live_ast.mpd", "", "");

  adaptive::SPINCACHE<adaptive::AdaptiveTree::Segment> segments =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segments_;

  EXPECT_EQ(segments.size(), 13);
  EXPECT_EQ(segments[0]->range_end_, 487050);
  EXPECT_EQ(segments[12]->range_end_, 487062);
}

TEST_F(DASHTreeTest, CalculateCorrectSegmentNumbersFromSegmentTemplateWithPTO)
{
  tree->mock_time = 1617223929L;

  OpenTestFile("mpd/segtpl_pto.mpd", "", "");

  adaptive::SPINCACHE<adaptive::AdaptiveTree::Segment> segments =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segments_;

  EXPECT_EQ(segments.size(), 451);
  EXPECT_EQ(segments[0]->range_end_, 404305525);
  EXPECT_EQ(segments[450]->range_end_, 404305975);
}

TEST_F(DASHTreeTest, CalculateCorrectSegmentNumbersFromSegmentTemplateWithOldPublishTime)
{
  tree->mock_time = 1617229334L;

  OpenTestFile("mpd/segtpl_old_publish_time.mpd", "", "");

  adaptive::SPINCACHE<adaptive::AdaptiveTree::Segment> segments =
      tree->periods_[0]->adaptationSets_[0]->representations_[0]->segments_;

  EXPECT_EQ(segments.size(), 31);
  EXPECT_EQ(segments[0]->range_end_, 603272);
  EXPECT_EQ(segments[30]->range_end_, 603302);
}

TEST_F(DASHTreeTest, CalculateLiveWithPresentationDuration)
{
  OpenTestFile("mpd/segtimeline_live_pd.mpd", "", "");
  EXPECT_EQ(tree->has_timeshift_buffer_, true);
}

TEST_F(DASHTreeTest, CalculateStaticWithPresentationDuration)
{
  OpenTestFile("mpd/segtpl_slash_baseurl_slash.mpd", "", "");
  EXPECT_EQ(tree->has_timeshift_buffer_, false);
}

TEST_F(DASHTreeTest, CalculateCorrectFpsScaleFromAdaptionSet)
{
  OpenTestFile("mpd/fps_scale_adaptset.mpd", "", "");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[0]->representations_[0]->fpsRate_, 24000);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[0]->representations_[0]->fpsScale_, 1001);

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[1]->representations_[0]->fpsRate_, 30);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[1]->representations_[0]->fpsScale_, 1);

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[2]->representations_[0]->fpsRate_, 25);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[2]->representations_[0]->fpsScale_, 1);

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[3]->representations_[0]->fpsRate_, 25000);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[3]->representations_[0]->fpsScale_, 1000);

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[4]->representations_[0]->fpsRate_, 25);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[4]->representations_[0]->fpsScale_, 1);

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[5]->representations_[0]->fpsRate_, 30);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[5]->representations_[0]->fpsScale_, 1);

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[6]->representations_[0]->fpsRate_, 25000);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[6]->representations_[0]->fpsScale_, 1000);
}

TEST_F(DASHTreeTest, replacePlaceHolders)
{
  OpenTestFile("mpd/placeholders.mpd", "https://foo.bar/placeholders.mpd", "");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[0], 0, 0, 0, 0, 0, 0, 0,
                              tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment_487050.m4s");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment_487054.m4s");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[1], 0, 0, 0, 0, 0, 0, 0,
                               tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment_00487050.m4s");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment_00487054.m4s");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[2], 0, 0, 0, 0, 0, 0, 0,
                               tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment_263007000000.m4s");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment_263009160000.m4s");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[3], 0, 0, 0, 0, 0, 0, 0,
                               tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment_00263007000000");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment_00263009160000");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[4], 0, 0, 0, 0, 0, 0, 0,
                               tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment_487050.m4s?t=263007000000");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment_487054.m4s?t=263009160000");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[5], 0, 0, 0, 0, 0, 0, 0,
                               tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment_00487050.m4s?t=00263007000000");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment_00487054.m4s?t=00263009160000");

  tree->videoStream->prepare_stream(tree->current_period_->adaptationSets_[6], 0, 0, 0, 0, 0, 0, 0,
                               tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/videosd-400x224/segment.m4s");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/videosd-400x224/segment.m4s");
}

TEST_F(DASHTreeTest, updateParameterLiveSegmentTimeline)
{
  OpenTestFile("mpd/segtimeline_live_pd.mpd", "", "");
  EXPECT_EQ(tree->update_parameter_, "full");
}

TEST_F(DASHTreeTest, updateParameterProvidedLiveSegmentTimeline)
{
  tree->update_parameter_ = "ABC";
  OpenTestFile("mpd/segtimeline_live_pd.mpd", "", "");
  EXPECT_EQ(tree->update_parameter_, "ABC");
}

TEST_F(DASHTreeTest, updateParameterVODSegmentTimeline)
{
  OpenTestFile("mpd/segtimeline_vod.mpd", "", "");
  EXPECT_EQ(tree->update_parameter_, "");
}

TEST_F(DASHTreeTest, updateParameterLiveSegmentTemplate)
{
  OpenTestFile("mpd/segtpl_pto.mpd", "", "");
  EXPECT_EQ(tree->update_parameter_, "");
}

TEST_F(DASHTreeTest, updateParameterVODSegmentTemplate)
{
  OpenTestFile("mpd/segtpl_baseurl_noslashs.mpd", "", "");
  EXPECT_EQ(tree->update_parameter_, "");
}

TEST_F(DASHTreeTest, CalculatePsshDefaultKid)
{
  OpenTestFile("mpd/pssh_default_kid.mpd", "", "");

  EXPECT_EQ(tree->periods_[0]->psshSets_[1].pssh_, "ABCDEFGH");
  EXPECT_EQ(tree->periods_[0]->psshSets_[1].defaultKID_.length(), 16);

  EXPECT_EQ(tree->periods_[0]->psshSets_[2].pssh_, "HGFEDCBA");
  EXPECT_EQ(tree->periods_[0]->psshSets_[2].defaultKID_.length(), 16);
}

TEST_F(DASHTreeTest, subtitles)
{
  OpenTestFile("mpd/subtitles.mpd", "https://foo.bar/subtitles.mpd", "");

  // Required as gtest can not access the hidden attribute directly in EXPECT_EQ
  static const uint16_t SUBTITLESTREAM = DASHTestTree::Representation::SUBTITLESTREAM;

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[1]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[1]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[1]->representations_[0]->codecs_, "ttml");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[2]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[2]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[2]->representations_[0]->codecs_, "ttml");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[3]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[3]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[3]->representations_[0]->codecs_, "ttml");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[4]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[4]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[4]->representations_[0]->codecs_, "ttml");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[5]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[5]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[5]->representations_[0]->codecs_, "wvtt");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[6]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[6]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[6]->representations_[0]->codecs_, "wvtt");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[7]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[7]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[7]->representations_[0]->codecs_, "wvtt");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[8]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[8]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[8]->representations_[0]->codecs_, "wvtt");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[9]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[9]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[9]->representations_[0]->codecs_, "my_codec");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[10]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[10]->representations_[0]->flags_, SUBTITLESTREAM);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[10]->representations_[0]->codecs_, "ttml");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[11]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[11]->mimeType_, "application/mp4");
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[11]->representations_[0]->codecs_, "stpp");
  tree->videoStream->prepare_stream(tree->periods_[0]->adaptationSets_[11], 0, 0, 0, 0, 0, 0, 0,
                              tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/11/0001.m4s");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/11/0005.m4s");

  EXPECT_EQ(tree->periods_[0]->adaptationSets_[12]->type_, DASHTestTree::SUBTITLE);
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[12]->mimeType_, "application/mp4");
  EXPECT_EQ(tree->periods_[0]->adaptationSets_[12]->representations_[0]->codecs_, "stpp.ttml.im1t");
  tree->videoStream->prepare_stream(tree->periods_[0]->adaptationSets_[12], 0, 0, 0, 0, 0, 0, 0,
                              tree->mediaHeaders);
  tree->videoStream->start_stream(~0, 0, 0, true);
  ReadSegments(tree->videoStream, 5);
  EXPECT_EQ(tree->downloadedUrls[0], "https://foo.bar/tears-of-steel-multiple-subtitles-12-0.dash");
  EXPECT_EQ(tree->downloadedUrls.back(), "https://foo.bar/tears-of-steel-multiple-subtitles-12-16000.dash");
}
