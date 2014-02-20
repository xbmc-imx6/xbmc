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
#include <imx-mm/vpu/vpu_wrapper.h>
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "DVDVideoCodecInfo.h"
#include "threads/CriticalSection.h"
#include "utils/BitstreamConverter.h"


//#define IMX_PROFILE
//#define TRACE_FRAMES

// FIXME TODO Develop real proper CVPUBuffer class
#define VPU_DEC_MAX_NUM_MEM_NUM 20
typedef struct
{
  //virtual mem info
  int nVirtNum;
  unsigned int virtMem[VPU_DEC_MAX_NUM_MEM_NUM];

  //phy mem info
  int nPhyNum;
  unsigned int phyMem_virtAddr[VPU_DEC_MAX_NUM_MEM_NUM];
  unsigned int phyMem_phyAddr[VPU_DEC_MAX_NUM_MEM_NUM];
  unsigned int phyMem_cpuAddr[VPU_DEC_MAX_NUM_MEM_NUM];
  unsigned int phyMem_size[VPU_DEC_MAX_NUM_MEM_NUM];
} DecMemInfo;

class CDVDVideoCodecIPUBuffer
{
public:
  CDVDVideoCodecIPUBuffer();

  uint8_t                *PhyAddr() const { return (uint8_t*)m_pPhyAddr; }
  uint8_t                *VirtAddr() const { return (uint8_t*)m_pVirtAddr; }

  // Returns whether the buffer is ready to be used
  bool                    IsAvail() const { return m_bAvail; }
  bool                    Process(int fd, int w, int h, VpuFieldType field, uint8_t *phyAddr);
  void                    Release() { m_bAvail = true; }

  bool                    Allocate(int fd, int width, int height, int nAlign);
  bool                    Free(int fd);

private:
  int                      m_pPhyAddr;
  void                    *m_pVirtAddr;
  int                      m_nWidth;
  int                      m_nHeight;
  int                      m_nSize;
  bool                     m_bAvail;
};

class CDVDVideoCodecIMXBuffer : public CDVDVideoCodecBuffer
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

  uint8_t                 *PhyAddr() const;
  uint8_t                 *VirtAddr() const;

  bool                     Rendered();
  void                     Queue(VpuFrameBuffer *buffer,
                                 CDVDVideoCodecIPUBuffer *ipuBuffer);
  VpuDecRetCode            ReleaseFramebuffer(VpuDecHandle *handle);
  void                     SetPts(double pts);
  double                   GetPts(void) const;

protected:
  // private because we are reference counted
  virtual                  ~CDVDVideoCodecIMXBuffer();

#ifdef TRACE_FRAMES
  int                      m_idx;
#endif
  long                     m_refs;
  VpuFrameBuffer          *m_frameBuffer;
  CDVDVideoCodecIPUBuffer *m_ipuBuffer;
  bool                     m_rendered;
  double                   m_pts;
};

class CDVDVideoCodecIPUBuffers
{
  public:
    CDVDVideoCodecIPUBuffers();
    ~CDVDVideoCodecIPUBuffers();

    bool Init(int width, int height, int numBuffers, int nAlign);
    bool Close();

    CDVDVideoCodecIPUBuffer *Process(int w, int h, VpuFieldType field, uint8_t *phyAddr);

  private:
    int                       m_ipuHandle;
    int                       m_bufferNum;
    CDVDVideoCodecIPUBuffer  *m_buffers;
};


class CDVDVideoCodecIMX : public CDVDVideoCodec
{
  friend class CDVDVideoCodecIMXBuffer;

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

  static const int    m_extraVpuBuffers;   // Number of additional buffers for VPU
  static CCriticalSection m_codecBufferLock;

  CDVDStreamInfo      m_hints;             // Hints from demuxer at stream opening
  const char         *m_pFormatName;       // Current decoder format name
  VpuDecOpenParam     m_decOpenParam;      // Parameters required to call VPU_DecOpen
  DecMemInfo          m_decMemInfo;        // VPU dedicated memory description
  VpuDecHandle        m_vpuHandle;         // Handle for VPU library calls
  VpuDecInitInfo      m_initInfo;          // Initial info returned from VPU at decoding start
  bool                m_dropState;         // Current drop state
  int                 m_vpuFrameBufferNum; // Total number of allocated frame buffers
  VpuFrameBuffer     *m_vpuFrameBuffers;   // Table of VPU frame buffers description
  CDVDVideoCodecIPUBuffers  m_deinterlacer;
  CDVDVideoCodecIMXBuffer **m_outputBuffers;
  VpuMemDesc         *m_extraMem;          // Table of allocated extra Memory
//  VpuMemDesc         *m_outputBuffers;     // Table of buffers out of VPU (used to call properly VPU_DecOutFrameDisplayed)
  int                 m_frameCounter;      // Decoded frames counter
  bool                m_usePTS;            // State whether pts out of decoding process should be used
  VpuDecOutFrameInfo  m_frameInfo;
  CBitstreamConverter *m_converter;
  bool                m_convert_bitstream;
};
