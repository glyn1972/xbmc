#include "DemuxTimeline.h"

#include <algorithm>

#include "DVDClock.h"
#include "Interface/DemuxPacket.h"
#include "DVDFactoryDemuxer.h"
#include "DVDDemuxFFmpeg.h"
#include "DVDInputStreams/DVDInputStreamFile.h"
#include "DVDInputStreams/DVDFactoryInputStream.h"
#include "filesystem/File.h"
#include "filesystem/Directory.h"
#include "MatroskaParser.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "ServiceBroker.h"
#include "settings/SettingsComponent.h"

// quick&dirty hack: use global variables for MKV edition switching
int g_currentEdition = 0;
int g_requestedEdition = 0;
bool g_multiEdition = false;

CDemuxTimeline::CDemuxTimeline() {}

CDemuxTimeline::~CDemuxTimeline() {}

bool CDemuxTimeline::SwitchToNextDemuxer()
{
  if (m_curChapter->index + 1 == m_chapters.size())
    return false;
  CLog::Log(LOGDEBUG, "TimelineDemuxer: Switch Demuxer");
  
  ChapterInfo *prevChapter = m_curChapter;
  m_curChapter = &m_chapters[m_curChapter->index + 1];

  if(prevChapter->demuxer->GetDemuxerId() == m_curChapter->demuxer->GetDemuxerId() && prevChapter->stopSrcTime() == m_curChapter->startSrcTime)
  {
    CLog::Log(LOGDEBUG, "TimelineDemuxer: No seek performed - demuxer is the same, and chapters run into each other.");
  }
  else
  {
    CLog::Log(LOGDEBUG, "TimelineDemuxer: Performing seek after demuxer switch");
    m_curChapter->demuxer->SeekTime(m_curChapter->startSrcTime, true);
  }
  
  return true;
}

bool CDemuxTimeline::Reset()
{
  for (auto &demuxer : m_demuxers)
    demuxer->Reset();
  m_curChapter = m_chapterMap.begin()->second;
  if (m_curChapter->startSrcTime != 0)
    m_curChapter->demuxer->SeekTime(m_curChapter->startSrcTime);
  return true;
}

void CDemuxTimeline::Abort()
{
  m_curChapter->demuxer->Abort();
}

void CDemuxTimeline::Flush()
{
  m_curChapter->demuxer->Flush();
}

DemuxPacket* CDemuxTimeline::Read()
{
  DemuxPacket *packet = nullptr;
  double pts = std::numeric_limits<double>::infinity();
  double dispPts;

  packet = m_curChapter->demuxer->Read();
  if (packet)
    pts = (packet->dts != DVD_NOPTS_VALUE ? packet->dts : packet->pts);

  while (
    !packet ||
    pts + packet->duration < DVD_MSEC_TO_TIME(m_curChapter->startSrcTime) ||
    pts >= DVD_MSEC_TO_TIME(m_curChapter->stopSrcTime())
  )
  {
    if (!packet || pts >= DVD_MSEC_TO_TIME(m_curChapter->stopSrcTime()))
      if (!SwitchToNextDemuxer())
        return nullptr;
    packet = m_curChapter->demuxer->Read();
    if (packet)
      pts = (packet->dts != DVD_NOPTS_VALUE ? packet->dts : packet->pts);
  }

  dispPts = pts + DVD_MSEC_TO_TIME(m_curChapter->shiftTime());
  packet->dts = dispPts;
  packet->pts = dispPts;
  packet->duration = std::min(packet->duration, DVD_MSEC_TO_TIME(m_curChapter->stopSrcTime()) - pts);
  packet->dispTime = DVD_TIME_TO_MSEC(pts) + m_curChapter->shiftTime();

  return packet;
}

bool CDemuxTimeline::SeekTime(double time, bool backwords, double* startpts)
{
  auto it = m_chapterMap.lower_bound(time);
  if (it == m_chapterMap.end())
    return false;

  CLog::Log(LOGDEBUG, "TimelineDemuxer: Switch Demuxer");
  m_curChapter = it->second;
  bool result = m_curChapter->demuxer->SeekTime(time - m_curChapter->shiftTime(), backwords, startpts);
  if (result && startpts)
    (*startpts) += m_curChapter->shiftTime();
  return result;
}

bool CDemuxTimeline::SeekChapter(int chapter, double* startpts)
{
  --chapter;
  if (chapter < 0 || unsigned(chapter) >= m_visibleChapters.size())
    return false;
  CLog::Log(LOGDEBUG, "TimelineDemuxer: Switch Demuxer");
  m_curChapter = &m_visibleChapters[chapter];
  bool result = m_curChapter->demuxer->SeekTime(m_curChapter->startSrcTime, true, startpts);
  if (result && startpts)
    (*startpts) += m_curChapter->shiftTime();
  return result;
}

int CDemuxTimeline::GetChapterCount()
{
  return m_visibleChapters.size();
}

int CDemuxTimeline::GetChapter()
{
  return m_visibleChapters->index + 1;
}

void CDemuxTimeline::GetChapterName(std::string& strChapterName, int chapterIdx)
{
  --chapterIdx;
  if (chapterIdx < 0 || unsigned(chapterIdx) >= m_visibleChapters.size())
    return;
  strChapterName = m_visibleChapters[chapterIdx].title;
}

int64_t CDemuxTimeline::GetChapterPos(int chapterIdx)
{
  --chapterIdx;
  if (chapterIdx < 0 || unsigned(chapterIdx) >= m_visibleChapters.size())
    return 0;
  return (m_visibleChapters[chapterIdx].startDispTime + 999) / 1000;
}

void CDemuxTimeline::SetSpeed(int iSpeed)
{
  for (auto &demuxer : m_demuxers)
    demuxer->SetSpeed(iSpeed);
}

int CDemuxTimeline::GetStreamLength()
{
  return m_chapterMap.rbegin()->first;
}

std::vector<CDemuxStream*> CDemuxTimeline::GetStreams() const
{
  return m_primaryDemuxer->GetStreams();
}

int CDemuxTimeline::GetNrOfStreams() const
{
  return m_primaryDemuxer->GetNrOfStreams();
}

std::string CDemuxTimeline::GetFileName()
{
  return m_primaryDemuxer->GetFileName();
}

void CDemuxTimeline::EnableStream(int id, bool enable)
{
  for (auto &demuxer : m_demuxers)
    demuxer->EnableStream(demuxer->GetDemuxerId(), id, enable);
}

CDemuxStream* CDemuxTimeline::GetStream(int iStreamId) const
{
  return m_primaryDemuxer->GetStream(m_primaryDemuxer->GetDemuxerId(), iStreamId);
}

std::string CDemuxTimeline::GetStreamCodecName(int iStreamId)
{
  return m_primaryDemuxer->GetStreamCodecName(m_primaryDemuxer->GetDemuxerId(), iStreamId);
}


std::string segUidToHex(std::string uid)
{
  const char *hex = "0123456789abcdef";
  std::string result("0x");
  result.reserve(18);
  for (unsigned char twoDigits : uid)
  {
    result.append(1, hex[(twoDigits >> 4) & 0xf]);
    result.append(1, hex[twoDigits & 0xf]);
  }
  return result;
}

CDemuxTimeline* CDemuxTimeline::CreateTimeline(CDVDDemux *primaryDemuxer)
{
  int requestedEdition = g_requestedEdition;
  // reset global variables to initialized state
  g_requestedEdition = 0;
  g_currentEdition = 1;
  g_multiEdition = false;

  std::unique_ptr<CDVDInputStreamFile> inStream(new CDVDInputStreamFile(CFileItem(primaryDemuxer->GetFileName(), false), 0));
  if (!inStream->Open())
    return nullptr;
  CDVDInputStream *input = inStream.get();

  MatroskaFile mkv;
  bool result = mkv.Parse(input);
  if (!result)
    return nullptr;

  // at least one edition is need
  if (mkv.segment.chapters.editions.size() == 0)
    return nullptr;
  if (mkv.segment.chapters.editions.size() > 1) { g_multiEdition = true; }

  // pre-set edition to fall-back mode: first non-hidden ordered or first available
  int i = 1;
  auto &edition = mkv.segment.chapters.editions.front();
  for (auto &e : mkv.segment.chapters.editions)
    if (e.flagOrdered)
    {
      edition = e;
      break;
    }
  for (auto &e : mkv.segment.chapters.editions)
    if (!e.flagHidden && e.flagOrdered)
    {
      edition = e;
      break;
    }
  // try to set default edition if no specific edition is requested
  if (!requestedEdition)
  {
    for (auto &e : mkv.segment.chapters.editions)
    {
      if (e.flagDefault && e.flagOrdered)
      {
        edition = e;
        CLog::Log(LOGINFO, "TimelineDemuxer: Found default edition %d", i);
        g_currentEdition = i;
        if(!e.flagHidden) break;
      }
      i++;
    }
  }
  // otherwise try to set edition as requested
  else
  {
    if (requestedEdition < 0) // last edition
    {
      for (auto &e : mkv.segment.chapters.editions)
        if (!e.flagHidden && e.flagOrdered) { edition = e; i++; }
      g_currentEdition = --i;
      CLog::Log(LOGINFO, "TimelineDemuxer: Found last edition %d", i);
    }
    else
    {
      for (auto &e : mkv.segment.chapters.editions)
        if (!e.flagHidden && e.flagOrdered)
        {
          if (i == requestedEdition)
          {
            edition = e;
            g_currentEdition = i;
            CLog::Log(LOGINFO, "TimelineDemuxer: Found requested edition %d", i);
            break;
          }
          i++;
        }
    }
  }
  // only handle ordered editions
  if (!edition.flagOrdered)
    return nullptr;

  std::unique_ptr<CDemuxTimeline> timeline(new CDemuxTimeline);
  timeline->m_primaryDemuxer = primaryDemuxer;
  timeline->m_demuxers.emplace_back(primaryDemuxer);

  // collect needed segment uids
  std::set<MatroskaSegmentUID> neededSegmentUIDs;
  for (auto &chapter : edition.chapterAtoms)
    if (chapter.segUid.size() != 0 && chapter.segUid != mkv.segment.infos.uid)
      neededSegmentUIDs.insert(chapter.segUid);

  // find linked segments
  std::map<MatroskaSegmentUID,CDVDDemux*> segmentDemuxer;
  segmentDemuxer[""] = primaryDemuxer;
  segmentDemuxer[mkv.segment.infos.uid] = primaryDemuxer;
  auto &searchDirs = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoMkvSegmentsSearchDirs

  std::string filename = primaryDemuxer->GetFileName();
  size_t slashPosition = filename.find_last_of("/\\");
  if (slashPosition == std::string::npos)
    return nullptr; // no directory

  std::string dirname = filename.substr(0, slashPosition + 1);

  for (auto &subDir : searchDirs)
  {
    if (neededSegmentUIDs.size() == 0)
      break;
    CFileItemList files;
    XFILE::CDirectory::GetDirectory(dirname + subDir, files, ".mkv", XFILE::DIR_FLAG_DEFAULTS);
    for (auto &file : files.GetList())
    {
      std::shared_ptr<CDVDInputStreamFile> uInput2(new CDVDInputStreamFile(*file, 0));
      CDVDInputStream *input2 = uInput2.get();
      if (!input2->Open())
        continue;
      MatroskaFile mkv2;
      if (!mkv2.Parse(input2))
        continue;
      if (neededSegmentUIDs.erase(mkv2.segment.infos.uid) == 0)
        continue;
      input2->Seek(mkv2.offsetBegin, SEEK_SET);
      std::unique_ptr<CDVDDemuxFFmpeg> demuxer(new CDVDDemuxFFmpeg());
      if(demuxer->Open(input2, false))
      {
        segmentDemuxer[mkv2.segment.infos.uid] = demuxer.get();
        timeline->m_demuxers.emplace_back(std::move(demuxer));
        timeline->m_inputStreams.emplace_back(std::move(uInput2));
      }
      if (neededSegmentUIDs.size() == 0)
        break;
    }
  }

  // build timeline
  for (auto &segUid : neededSegmentUIDs)
    CLog::Log(LOGERROR,
      "TimelineDemuxer: Could not find matroska segment for segment linking: %s",
      segUidToHex(segUid).c_str()
    );

  int dispTime = 0;
  decltype(segmentDemuxer.begin()) it;
  for (auto &chapter : edition.chapterAtoms)
    if ((it = segmentDemuxer.find(chapter.segUid)) != segmentDemuxer.end())
    {
      timeline->m_chapters.emplace_back(
        it->second,
        chapter.timeStart / 1000000,
        dispTime,
        (chapter.timeEnd - chapter.timeStart) / 1000000,
        timeline->m_chapters.size(),
        chapter.displays.GetDefault()
      );
      dispTime += timeline->m_chapters.back().duration;
      if(!chapter.flagHidden)
        timeline->m_visibleChapters.emplace_back(m_chapters.back());
    }

  if (!timeline->m_chapters.size())
    return nullptr;

  for (auto &chapter : timeline->m_chapters)
    timeline->m_chapterMap[chapter.stopDispTime() - 1] = &chapter;

  timeline->m_curChapter = &timeline->m_chapters.front();
  return timeline.release();
}

// vim: ts=2 sw=2 expandtab