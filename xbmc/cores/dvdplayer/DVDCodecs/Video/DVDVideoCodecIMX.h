#pragma once
/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://www.xbmc.org
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
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */
#include <queue>
#include <vector>
#include <imx-mm/vpu/vpu_wrapper.h>
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "DVDVideoCodecInfo.h"
#include "threads/CriticalSection.h"
#include "threads/Condition.h"
#include "threads/Event.h"
#include "threads/Thread.h"
#include "utils/BitstreamConverter.h"


// The decoding format of the VPU buffer. Comment this to decode
// as NV12. The VPU works faster with I420.
#define IMX_INPUT_FORMAT_I420

// The deinterlacer output and render format. Comment to use NV12.
// The IPU works faster when outputting to NV12
//#define IMX_OUTPUT_FORMAT_I420

#define IMX_PROFILE_BUFFERS
//#define IMX_PROFILE
//#define TRACE_FRAMES

class CDecMemInfo
{
public:
  CDecMemInfo()
    : nVirtNum(0)
    , virtMem(NULL)
    , nPhyNum(0)
    , phyMem(NULL)
  {}

  //virtual mem info
  int nVirtNum;
  void** virtMem;

  //phy mem info
  int nPhyNum;
  VpuMemDesc* phyMem;
};


class CDVDVideoCodecBufferTs : public CDVDVideoCodecBuffer {
public:
  CDVDVideoCodecBufferTs();

  void    SetPts(double pts);
  double  GetPts(void) const;

  void    SetDts(double dts);
  double  GetDts(void) const;

private:
  double  m_pts;
  double  m_dts;
};

class CDVDVideoCodecIPUBuffer;

class CDVDVideoCodecIMXBuffer : public CDVDVideoCodecBufferTs
{
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIMXBuffer(int idx);
#else
  CDVDVideoCodecIMXBuffer();
#endif

  // reference counting
  virtual void             Lock();
  virtual long             Release();
  virtual bool             IsValid();

  bool                     Rendered() const;
  void                     Queue(VpuDecOutFrameInfo *frameInfo,
                                 CDVDVideoCodecIMXBuffer *previous);
  VpuDecRetCode            ReleaseFramebuffer(VpuDecHandle *handle);
  CDVDVideoCodecIMXBuffer *GetPreviousBuffer() const;

private:
  // private because we are reference counted
  virtual                  ~CDVDVideoCodecIMXBuffer();

private:
#ifdef TRACE_FRAMES
  int                      m_idx;
#endif
  long                     m_refs;
  VpuFrameBuffer          *m_frameBuffer;
  bool                     m_rendered;
  CDVDVideoCodecIMXBuffer *m_previousBuffer; // Holds a the reference counted
                                             // previous buffer
};

// Shared buffer that holds an IPU allocated memory block and serves as target
// for IPU operations such as deinterlacing, rotation or color conversion.
class CDVDVideoCodecIPUBuffer : public CDVDVideoCodecBufferTs
{
public:
#ifdef TRACE_FRAMES
  CDVDVideoCodecIPUBuffer(int idx);
#else
  CDVDVideoCodecIPUBuffer();
#endif

  // reference counting
  virtual void             Lock();
  virtual long             Release();
  virtual bool             IsValid();

  // Returns whether the buffer is ready to be used
  bool                     Rendered() const { return m_bFree; }
  bool                     Process(int fd, CDVDVideoCodecIMXBuffer *buffer,
                                   VpuFieldType fieldType, int fieldFmt,
                                   bool lowMotion);
  void                     ReleaseFrameBuffer();

  bool                     Allocate(int fd, int width, int height, int nAlign);
  bool                     Free(int fd);

private:
  virtual                  ~CDVDVideoCodecIPUBuffer();

private:
#ifdef TRACE_FRAMES
  int                      m_idx;
#endif
  long                     m_refs;
  bool                     m_bFree;
  int                      m_pPhyAddr;
  uint8_t                 *m_pVirtAddr;
  int                      m_iWidth;
  int                      m_iHeight;
  int                      m_nSize;
};

// Collection class that manages a pool of IPU buffers that are used for
// deinterlacing. In future they can also serve rotation or color conversion
// buffers.
class CDVDVideoCodecIPUBuffers
{
  public:
    CDVDVideoCodecIPUBuffers();
    ~CDVDVideoCodecIPUBuffers();

    bool Init(int width, int height, int numBuffers, int nAlign);
    // Sets the mode to be used if deinterlacing is set to AUTO
    void SetAutoMode(bool mode) { m_autoMode = mode; }
	bool AutoMode() const { return m_autoMode; }
    bool Reset();
    bool Close();

    CDVDVideoCodecIPUBuffer *Process(CDVDVideoCodecBuffer *sourceBuffer,
                                     VpuFieldType fieldType, bool lowMotion);

  private:
    int                       m_ipuHandle;
	bool                      m_autoMode;
    int                       m_bufferNum;
    CDVDVideoCodecIPUBuffer **m_buffers;
    int                       m_currentFieldFmt;
};


class CDVDVideoMixerIMX : private CThread {
public:
  CDVDVideoMixerIMX(CDVDVideoCodecIPUBuffers *proc);
  virtual ~CDVDVideoMixerIMX();

  void SetCapacity(int intput, int output);

  void Start();
  void Reset();
  void Dispose();
  bool IsActive();

  // This function blocks until an input slot is available.
  // It returns if an output is available.
  CDVDVideoCodecBufferTs *Process(CDVDVideoCodecIMXBuffer *input);

private:
  CDVDVideoCodecIMXBuffer *GetNextInput();
  void WaitForFreeOutput();
  bool PushOutput(CDVDVideoCodecBufferTs *v);
  CDVDVideoCodecBufferTs *ProcessFrame(CDVDVideoCodecIMXBuffer *input);

  virtual void OnStartup();
  virtual void OnExit();
  virtual void StopThread(bool bWait = true);
  virtual void Process();

private:
  typedef std::vector<CDVDVideoCodecIMXBuffer*> InputBuffers;
  typedef std::vector<CDVDVideoCodecBufferTs*> OutputBuffers;

  CDVDVideoCodecIPUBuffers       *m_proc;
  InputBuffers                    m_input;
  volatile int                    m_beginInput, m_endInput;
  volatile size_t                 m_bufferedInput;
  XbmcThreads::ConditionVariable  m_inputNotEmpty;
  XbmcThreads::ConditionVariable  m_inputNotFull;

  OutputBuffers                   m_output;
  volatile int                    m_beginOutput, m_endOutput;
  volatile size_t                 m_bufferedOutput;
  XbmcThreads::ConditionVariable  m_outputNotFull;

  mutable CCriticalSection        m_monitor;
  CDVDVideoCodecBufferTs         *m_lastFrame;    // Last input frame
};


class CDVDVideoCodecIMX : public CDVDVideoCodec
{
  friend class CDVDVideoCodecIMXBuffer;
  friend class CDVDVideoCodecIPUBuffer;

public:
  CDVDVideoCodecIMX();
  virtual ~CDVDVideoCodecIMX();

  // Methods from CDVDVideoCodec which require overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose(void);
  virtual int  Decode(BYTE *pData, int iSize, double dts, double pts);
  virtual void Reset(void);
  virtual bool ClearPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual void SetDropState(bool bDrop);
  virtual const char* GetName(void) { return (const char*)m_pFormatName; }
  virtual unsigned GetAllowedReferences();

protected:

  bool VpuOpen();
  bool VpuAllocBuffers(VpuMemInfo *);
  bool VpuFreeBuffers();
  bool VpuAllocFrameBuffers();
  int  VpuFindBuffer(void *frameAddr);

  static const int          m_extraVpuBuffers;   // Number of additional buffers for VPU
  static const int          m_maxVpuDecodeLoops; // Maximum iterations in VPU decoding loop
                                                 // by both decoding and rendering threads

  CDVDStreamInfo            m_hints;             // Hints from demuxer at stream opening
  const char               *m_pFormatName;       // Current decoder format name
  VpuDecOpenParam           m_decOpenParam;      // Parameters required to call VPU_DecOpen
  CDecMemInfo               m_decMemInfo;        // VPU dedicated memory description
  VpuDecHandle              m_vpuHandle;         // Handle for VPU library calls
  VpuDecInitInfo            m_initInfo;          // Initial info returned from VPU at decoding start
  bool                      m_dropState;         // Current drop state
  int                       m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer           *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  CDVDVideoCodecIPUBuffers  m_deinterlacer;      // Pool of buffers used for deinterlacing
  CDVDVideoMixerIMX         m_mixer;
  CDVDVideoCodecIMXBuffer **m_outputBuffers;     // Table of VPU output buffers
  CDVDVideoCodecIMXBuffer  *m_lastBuffer;        // Keep track of previous VPU output buffer (needed by deinterlacing motion engin)
  CDVDVideoCodecBufferTs   *m_currentBuffer;
  VpuMemDesc               *m_extraMem;          // Table of allocated extra Memory
  int                       m_frameCounter;      // Decoded frames counter
  bool                      m_usePTS;            // State whether pts out of decoding process should be used
  VpuDecOutFrameInfo        m_frameInfo;         // Store last VPU output frame info
  CBitstreamConverter      *m_converter;         // H264 annex B converter
  bool                      m_convert_bitstream; // State whether bitstream conversion is required
  int                       m_bytesToBeConsumed; // Remaining bytes in VPU
  double                    m_previousPts;       // Enable to keep pts when needed
  bool                      m_frameReported;     // State whether the frame consumed event will be reported by libfslvpu
};
