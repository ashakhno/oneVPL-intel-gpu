// Copyright (c) 2022 Intel Corporation
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "mfx_common.h"

#if defined (ONEVPL_EXPERIMENTAL)

#include "mfx_perc_enc_vpp.h"
#include "mfx_ext_buffers.h"
#include "mfx_common_int.h"

namespace PercEncPrefilter
{

mfxStatus PercEncFilter::Query(mfxExtBuffer* hint)
{
    std::ignore = hint;
    return MFX_ERR_NONE;
}

PercEncFilter::PercEncFilter(VideoCORE* pCore, mfxVideoParam const& par)
{
    m_core = dynamic_cast <CommonCORE_VPL*>(pCore);
    MFX_CHECK_WITH_THROW_STS(m_core, MFX_ERR_NULL_PTR);
    std::ignore = par;
}

PercEncFilter::~PercEncFilter()
{
    std::ignore = Close();
}

mfxStatus PercEncFilter::Init(mfxFrameInfo* in, mfxFrameInfo* out)
{
    const bool cpuHasAvx2 = __builtin_cpu_supports("avx2");
    MFX_CHECK(cpuHasAvx2, MFX_ERR_UNSUPPORTED)

    MFX_CHECK_NULL_PTR1(in);
    MFX_CHECK_NULL_PTR1(out);

    if (m_initialized)
        return MFX_ERR_NONE;

    MFX_CHECK(in->CropW          == out->CropW,          MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->CropH          == out->CropH,          MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->FourCC         == out->FourCC,         MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->BitDepthLuma   == out->BitDepthLuma,   MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->BitDepthChroma == out->BitDepthChroma, MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->ChromaFormat   == out->ChromaFormat,   MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->Shift          == out->Shift,          MFX_ERR_INVALID_VIDEO_PARAM);

    MFX_CHECK(in->CropW >= 16, MFX_ERR_INVALID_VIDEO_PARAM);
    MFX_CHECK(in->CropH >= 2, MFX_ERR_INVALID_VIDEO_PARAM);

    width = in->CropW;
    height = in->CropH;
    previousOutput.resize(width * height);
    filter = std::make_unique<Filter>(parametersFrame, parametersBlock, width);

#if defined(MFX_ENABLE_ENCTOOLS)
    //modulation map
    m_frameCounter = 0;
    m_saliencyMapSupported = false;

    mfxVideoParam par{};
    m_encTools = MFXVideoENCODE_CreateEncTools(par);

    if(m_encTools)
    {
        mfxExtEncToolsConfig config{};
        mfxEncToolsCtrl ctrl{};

        config.SaliencyMapHint = MFX_CODINGOPTION_ON;
        ctrl.CodecId = MFX_CODEC_AVC;

        mfxEncToolsCtrlExtAllocator extAllocBut{};
        extAllocBut.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_ALLOCATOR;
        extAllocBut.Header.BufferSz = sizeof(mfxEncToolsCtrlExtAllocator);

        mfxFrameAllocator* pFrameAlloc = QueryCoreInterface<mfxFrameAllocator>(m_core, MFXIEXTERNALLOC_GUID);
        MFX_CHECK_NULL_PTR1(pFrameAlloc);
        extAllocBut.pAllocator = pFrameAlloc;

        std::vector<mfxExtBuffer*> extParams;
        extParams.push_back(&extAllocBut.Header);

        ctrl.ExtParam = extParams.data();
        ctrl.NumExtParam = (mfxU16)extParams.size();

        ctrl.FrameInfo.CropH = in->CropH;
        ctrl.FrameInfo.CropW = in->CropW;

        mfxStatus sts = m_encTools->Init(m_encTools->Context, &config, &ctrl);
        m_saliencyMapSupported = (sts == MFX_ERR_NONE);
    }
#endif
    m_initialized = true;

    return MFX_ERR_NONE;
}

mfxStatus PercEncFilter::Close()
{
#if defined(MFX_ENABLE_ENCTOOLS)
    if(m_encTools)
    {
        m_encTools->Close(m_encTools->Context);
        MFXVideoENCODE_DestroyEncTools(m_encTools);
    }
#endif
    return MFX_ERR_NONE;
}

mfxStatus PercEncFilter::Reset(mfxVideoParam* video_param)
{
    MFX_CHECK_NULL_PTR1(video_param);

    MFX_SAFE_CALL(Close());

    MFX_SAFE_CALL(Init(&video_param->vpp.In, &video_param->vpp.Out));

    return MFX_ERR_NONE;
}

mfxStatus PercEncFilter::SetParam(mfxExtBuffer*)
{
    return MFX_ERR_NONE;
}

mfxStatus PercEncFilter::RunFrameVPPTask(mfxFrameSurface1* in, mfxFrameSurface1* out, InternalParam* param)
{
    return RunFrameVPP(in, out, param);
}

mfxStatus PercEncFilter::RunFrameVPP(mfxFrameSurface1* in, mfxFrameSurface1* out, InternalParam*)
{
    MFX_CHECK_NULL_PTR1(in);
    MFX_CHECK_NULL_PTR1(out);

    //skip filtering if cropping or resizing is required
    if( in->Info.CropX != out->Info.CropX || in->Info.CropX != 0 ||
        in->Info.CropY != out->Info.CropY || in->Info.CropY != 0 ||
        in->Info.CropW != out->Info.CropW ||
        in->Info.CropH != out->Info.CropH ||
        in->Data.Pitch != out->Data.Pitch
    ){
        return MFX_ERR_NONE;
    }
#if defined(MFX_ENABLE_ENCTOOLS)
    //get modulation map
    mfxStatus sts = MFX_ERR_NONE;

    if(m_saliencyMapSupported)
    {
        {   mfxEncToolsFrameToAnalyze extFrameData = {};
            extFrameData.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_FRAME_TO_ANALYZE;
            extFrameData.Header.BufferSz = sizeof(extFrameData);
            extFrameData.Surface = in;

            std::vector<mfxExtBuffer*> extParams;
            extParams.push_back(&extFrameData.Header);

            mfxEncToolsTaskParam param{};
            param.ExtParam = extParams.data();
            param.NumExtParam = (mfxU16)extParams.size();
            param.DisplayOrder = m_frameCounter;

            sts = m_encTools->Submit(m_encTools->Context, &param);
            MFX_CHECK_STS(sts);
        }

        {   mfxEncToolsHintSaliencyMap extSM = {};
            extSM.Header.BufferId = MFX_EXTBUFF_ENCTOOLS_HINT_SALIENCY_MAP;
            extSM.Header.BufferSz = sizeof(extSM);

            mfxU32 blockSize = 8;
            mfxU32 numOfBlocks = in->Info.Width * in->Info.Height / (blockSize * blockSize);
            std::unique_ptr<mfxF32[]> smBuffer(new mfxF32[numOfBlocks]);

            extSM.AllocatedSize = numOfBlocks;
            extSM.SaliencyMap = smBuffer.get();

            std::vector<mfxExtBuffer*> extParams;
            extParams.push_back(&extSM.Header);

            mfxEncToolsTaskParam param{};
            param.ExtParam = extParams.data();
            param.NumExtParam = (mfxU16)extParams.size();
            param.DisplayOrder = m_frameCounter;
            m_frameCounter++;

            sts = m_encTools->Query(m_encTools->Context, &param, 0 /*timeout*/);
            MFX_CHECK_STS(sts);
        }
    }
#endif

    mfxFrameSurface1_scoped_lock inLock(in, m_core), outLock(out, m_core);
    MFX_SAFE_CALL(inLock.lock(MFX_MAP_READ));
    MFX_SAFE_CALL(outLock.lock(MFX_MAP_WRITE));

    if (filter)
    {
        filter->processFrame(in->Data.Y, in->Data.Pitch, modulation.data(), modulationStride, previousOutput.data(), width, out->Data.Y, out->Data.Pitch, width, height);
    }
    else
    {
        for (int y = 0; y < height; ++y)
            std::copy(
                &in->Data.Y[out->Data.Pitch * y],
                &in->Data.Y[out->Data.Pitch * y + width],
                &out->Data.Y[out->Data.Pitch * y]);
    }

    // retain a copy of the output for next time... (it would be nice to avoid this copy)
    for (int y = 0; y < height; ++y)
        std::copy(
            &out->Data.Y[out->Data.Pitch * y],
            &out->Data.Y[out->Data.Pitch * y + width],
            &previousOutput[width * y]);

    // copy chroma
    std::copy(
        &in->Data.UV[0],
        &in->Data.UV[in->Data.Pitch * height / 2],
        &out->Data.UV[0]);

    return MFX_ERR_NONE;
}

bool PercEncFilter::IsReadyOutput(mfxRequestType)
{
    //TBD: temporary do processing in sync. part therefore always return true
    return true;
}

}//namespace

#endif
