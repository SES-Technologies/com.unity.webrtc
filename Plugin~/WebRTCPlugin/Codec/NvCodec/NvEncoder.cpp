#include "pch.h"
#include "NvEncoder.h"
#include "Context.h"
#include <cstring>
#include "GraphicsDevice/IGraphicsDevice.h"
#include "HWSettings.h"
#include <iostream>
#include "Debugger.h"
#if _WIN32
#else
#include <dlfcn.h>
#endif

namespace unity
{
    namespace webrtc
    {

        static void* s_hModule = nullptr;
        static std::unique_ptr<NV_ENCODE_API_FUNCTION_LIST> pNvEncodeAPI;

        NvEncoder::NvEncoder(
            const NV_ENC_DEVICE_TYPE type,
            const NV_ENC_INPUT_RESOURCE_TYPE inputType,
            const NV_ENC_BUFFER_FORMAT bufferFormat,
            const int width, const int height, IGraphicsDevice* device)
            : m_width(width)
            , m_height(height)
            , m_device(device)
            , m_deviceType(type)
            , m_inputType(inputType)
            , m_bufferFormat(bufferFormat)
            , m_clock(webrtc::Clock::GetRealTimeClock())
        {
            LogPrint(StringFormat("width is %d, height is %d", width, height).c_str());
            checkf(width > 0 && height > 0, "Invalid width or height!");
        }

        void NvEncoder::InitV()
        {


            bool result = true;
            if (m_initializationResult == CodecInitializationResult::NotInitialized)
            {
                m_initializationResult = LoadCodec();
            }
            if (m_initializationResult != CodecInitializationResult::Success)
            {
                throw m_initializationResult;
            }
#pragma region open an encode session
            //open an encode session
            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openEncodeSessionExParams = { 0 };
            openEncodeSessionExParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;

            openEncodeSessionExParams.device = m_device->GetEncodeDevicePtrV();
            openEncodeSessionExParams.deviceType = m_deviceType;
            openEncodeSessionExParams.apiVersion = NVENCAPI_VERSION;
            errorCode = pNvEncodeAPI->nvEncOpenEncodeSessionEx(&openEncodeSessionExParams, &pEncoderInterface);

            if (!NV_RESULT(errorCode))
            {
                m_initializationResult = CodecInitializationResult::EncoderInitializationFailed;
                return;
            }

            checkf(NV_RESULT(errorCode), StringFormat("Unable to open NvEnc encode session %d", errorCode).c_str());
#pragma endregion
#pragma region set initialization parameters
            nvEncInitializeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
            nvEncInitializeParams.encodeWidth = m_width;
            nvEncInitializeParams.encodeHeight = m_height;
            nvEncInitializeParams.darWidth = m_width;
            nvEncInitializeParams.darHeight = m_height;
            nvEncInitializeParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
            nvEncInitializeParams.presetGUID = NV_ENC_PRESET_LOW_LATENCY_HP_GUID; // [autr] switch to HP
            nvEncInitializeParams.frameRateDen = 1;
            nvEncInitializeParams.enablePTD = 1;
            nvEncInitializeParams.reportSliceOffsets = 0;
            nvEncInitializeParams.enableSubFrameWrite = 0;
            nvEncInitializeParams.encodeConfig = &nvEncConfig;
            nvEncInitializeParams.maxEncodeWidth = 3840;
            nvEncInitializeParams.maxEncodeHeight = 2160;
#pragma endregion
#pragma region get preset ocnfig and set it
            NV_ENC_PRESET_CONFIG presetConfig = { 0 };
            presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
            presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
            errorCode = pNvEncodeAPI->nvEncGetEncodePresetConfig(pEncoderInterface, nvEncInitializeParams.encodeGUID, nvEncInitializeParams.presetGUID, &presetConfig);
            checkf(NV_RESULT(errorCode), StringFormat("Failed to select NVEncoder preset config %d", errorCode).c_str());
            std::memcpy(&nvEncConfig, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
            nvEncConfig.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;



            // Intra Refresh causes consecutive sections of the frames to be encoded using intra macroblocks, over intraRefreshCnt consecutive frames.Then the whole cycle repeats after intraRefreshPeriod frames from the first intra - refresh frame. It is essential to set intraRefreshPeriod and intraRefreshCnt appropriately based on the probability of errors that may occur during transmission. For example, intraRefreshPeriod may be small like 30 for a highly error prone network thus enabling recovery every second for a 30 FPS video stream.For networks that have lesser chances of error, the value may be set higher.Lower value of intraRefreshPeriod comes with a slightly lower quality as a larger portion of the overall macroblocks in an intra refresh period are forced to be intra coded, but provides faster recovery from network errors.

            // intraRefreshCnt determines the number of frames over which the intra refresh will happen within an intra refresh period. A smaller value of intraRefreshCnt will refresh the entire frame quickly(instead of refreshing it slowly in bands) and hence enable a faster error recovery. However, a lower intraRefreshCnt also means sending a larger number of intra macroblocks per frameand hence slightly lower quality.

            // Low - latency use cases like game - streaming, video conferencing etc.

            //    Ultra - low latency or low latency Tuning Info
            //    Rate control mode = CBR
            //    Multi Pass – Quarter / Full(evaluate and decide)
            //    Very low VBV buffer size(e.g.single frame = bitrate / framerate)
            //    No B Frames
            //    Infinite GOP length
            //    Adaptive quantization(AQ) enabled * *
            //    Long term reference pictures * **
            //    Intra refresh * **
            //    Non - reference P frames * **
            //    Force IDR *

            //* : Recommended for low motion gamesand natural video.
            //** : Recommended on second generation Maxwell GPUs and above.
            //*** : These features are useful for error recovery during transmission across noisy mediums.

            // https://docs.nvidia.com/video-technologies/video-codec-sdk/nvenc-video-encoder-api-prog-guide/index.html
            // http://developer.download.nvidia.com/compute/nvenc/v4.0/NVENC_AppNote.pdf

            //---------------------------------------------------------------------------------------------


            nvEncConfig.rcParams.averageBitRate =
                (static_cast<unsigned int>(5.0f *
                    nvEncInitializeParams.encodeWidth *
                    nvEncInitializeParams.encodeHeight) / (m_width * m_height)) * 100000;

            nvEncConfig.encodeCodecConfig.h264Config.sliceMode = 0;
            nvEncConfig.encodeCodecConfig.h264Config.sliceModeData = 0;
            nvEncConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
            //Quality Control
            nvEncConfig.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_51;

            //---------------------------------------------------------------------------------------------

            // [autr] begin...

            HWSettings* hw = HWSettings::getPtr();
            HWSettings& hw_ = *hw;

            // Set RCM

            nvEncConfig.rcParams.rateControlMode = GetRateControlMode(hw->rateControlMode); // [autr] default CBR

            // Set FPS

            //m_frameRate = hw->minFramerate; // TODO: investigate why setting this causes more glitches
            nvEncInitializeParams.frameRateNum = m_frameRate;

            // Optimise: infinite or FPS gop length

            nvEncConfig.encodeCodecConfig.h264Config.idrPeriod = m_frameRate;
            nvEncConfig.gopLength = (hw->infiniteGOP) ? NVENC_INFINITE_GOPLENGTH : m_frameRate; // NVENC_INFINITE_GOPLENGTH

            // Error Recovery Settings: infra frame refreshing

            if (hw->intraRefreshPeriod > 0 && hw->intraRefreshCount > 0) {

                nvEncConfig.encodeCodecConfig.h264Config.enableIntraRefresh = true;
                nvEncConfig.encodeCodecConfig.h264Config.intraRefreshPeriod = hw->intraRefreshPeriod;
                nvEncConfig.encodeCodecConfig.h264Config.intraRefreshCnt = hw->intraRefreshCount;
            }

            // Optimise: bitrates

            if (hw->minBitrate > 0) nvEncConfig.rcParams.averageBitRate = hw->minBitrate; // used with CBR, VBR etc (will override Unity's default calculation)
            if (hw->maxBitrate > 0) nvEncConfig.rcParams.maxBitRate = hw->maxBitrate; // VBR only

            // Error Recovery Settings: adaptive quantization

            if (hw->enableAQ) nvEncConfig.rcParams.enableAQ = true;
            if (hw->maxNumRefFrames > 0) nvEncConfig.encodeCodecConfig.h264Config.maxNumRefFrames = hw->maxNumRefFrames;  // zero will use driver's default size

            // Optimise: quantisation parameters

            if (hw->minQP > 0) {
                nvEncConfig.rcParams.enableMinQP = true;
                nvEncConfig.rcParams.minQP.qpIntra = nvEncConfig.rcParams.minQP.qpInterP = nvEncConfig.rcParams.minQP.qpInterB = hw->minQP;
            }
            if (hw->maxQP > 0) {
                nvEncConfig.rcParams.enableMaxQP = true;
                nvEncConfig.rcParams.maxQP.qpIntra = nvEncConfig.rcParams.maxQP.qpInterP = nvEncConfig.rcParams.maxQP.qpInterB = hw->maxQP;
            }

            // Error Recovery Settings: long term reference

            //nvEncConfig.encodeCodecConfig.h264Config.enableLTR = 1;
            //nvEncConfig.encodeCodecConfig.h264Config.ltrTrustMode = 1;
            //nvEncConfig.encodeCodecConfig.h264Config.ltrNumFrames = 8;


#pragma endregion


            // [/autr] end.

#pragma region get encoder capability
            NV_ENC_CAPS_PARAM capsParam = { 0 };
            capsParam.version = NV_ENC_CAPS_PARAM_VER;
            capsParam.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
            int32 asyncMode = 0;
            errorCode = pNvEncodeAPI->nvEncGetEncodeCaps(pEncoderInterface, nvEncInitializeParams.encodeGUID, &capsParam, &asyncMode);
            checkf(NV_RESULT(errorCode), StringFormat("Failed to get NVEncoder capability params %d", errorCode).c_str());
            nvEncInitializeParams.enableEncodeAsync = 0;
#pragma endregion
#pragma region initialize hardware encoder session
            errorCode = pNvEncodeAPI->nvEncInitializeEncoder(pEncoderInterface, &nvEncInitializeParams);
            result = NV_RESULT(errorCode);
            checkf(result, StringFormat("Failed to initialize NVEncoder %d", errorCode).c_str());
#pragma endregion
            InitEncoderResources();
            m_isNvEncoderSupported = true;
        }

        NvEncoder::~NvEncoder()
        {
            ReleaseEncoderResources();
            if (pEncoderInterface)
            {
                errorCode = pNvEncodeAPI->nvEncDestroyEncoder(pEncoderInterface);
                checkf(NV_RESULT(errorCode), StringFormat("Failed to destroy NV encoder interface %d", errorCode).c_str());
                pEncoderInterface = nullptr;
            }
        }

        CodecInitializationResult NvEncoder::LoadCodec()
        {
            pNvEncodeAPI = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
            pNvEncodeAPI->version = NV_ENCODE_API_FUNCTION_LIST_VER;

            if (!LoadModule())
            {
                return CodecInitializationResult::DriverNotInstalled;
            }
            using NvEncodeAPIGetMaxSupportedVersion_Type = NVENCSTATUS(NVENCAPI*)(uint32_t*);
#if defined(_WIN32)
            NvEncodeAPIGetMaxSupportedVersion_Type NvEncodeAPIGetMaxSupportedVersion = (NvEncodeAPIGetMaxSupportedVersion_Type)GetProcAddress((HMODULE)s_hModule, "NvEncodeAPIGetMaxSupportedVersion");
#else
            NvEncodeAPIGetMaxSupportedVersion_Type NvEncodeAPIGetMaxSupportedVersion = (NvEncodeAPIGetMaxSupportedVersion_Type)dlsym(s_hModule, "NvEncodeAPIGetMaxSupportedVersion");
#endif

            uint32_t version = 0;
            uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
            NvEncodeAPIGetMaxSupportedVersion(&version);
            if (currentVersion > version)
            {
                LogPrint("Current Driver Version does not support this NvEncodeAPI version, please upgrade driver");
                return CodecInitializationResult::DriverVersionDoesNotSupportAPI;
            }

            using NvEncodeAPICreateInstance_Type = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
#if defined(_WIN32)
            NvEncodeAPICreateInstance_Type NvEncodeAPICreateInstance = (NvEncodeAPICreateInstance_Type)GetProcAddress((HMODULE)s_hModule, "NvEncodeAPICreateInstance");
#else
            NvEncodeAPICreateInstance_Type NvEncodeAPICreateInstance = (NvEncodeAPICreateInstance_Type)dlsym(s_hModule, "NvEncodeAPICreateInstance");
#endif

            if (!NvEncodeAPICreateInstance)
            {
                LogPrint("Cannot find NvEncodeAPICreateInstance() entry in NVENC library");
                return CodecInitializationResult::APINotFound;
            }
            bool result = (NvEncodeAPICreateInstance(pNvEncodeAPI.get()) == NV_ENC_SUCCESS);
            checkf(result, "Unable to create NvEnc API function list");
            if (!result)
            {
                return CodecInitializationResult::APINotFound;
            }
            return CodecInitializationResult::Success;
        }

        bool NvEncoder::LoadModule()
        {
            if (s_hModule != nullptr)
                return true;

#if defined(_WIN32)
#if defined(_WIN64)
            HMODULE module = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
#else
            HMODULE module = LoadLibrary(TEXT("nvEncodeAPI.dll"));
#endif
#else
            void* module = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
#endif

            if (module == nullptr)
            {
                LogPrint("NVENC library file is not found. Please ensure NV driver is installed");
                return false;
            }
            s_hModule = module;
            return true;
        }

        void NvEncoder::UnloadModule()
        {
            if (s_hModule)
            {
#if defined(_WIN32)
                FreeLibrary((HMODULE)s_hModule);
#else
                dlclose(s_hModule);
#endif
                s_hModule = nullptr;
            }
        }

        void NvEncoder::UpdateSettings()
        {
            bool settingChanged = false;

            if (nvEncConfig.rcParams.averageBitRate != m_targetBitrate)
            {

                Debugger::Log("averageBitrate", m_targetBitrate);

                nvEncConfig.rcParams.averageBitRate = m_targetBitrate;

                settingChanged = true;
            }
            if (nvEncInitializeParams.frameRateNum != m_frameRate)
            {
                HWSettings* hw = HWSettings::getPtr();

                if (m_frameRate > 240) m_frameRate = 240; // unlikely: nvcodec do not allow a framerate over 240
                //if (m_frameRate > hw->maxFramerate) m_frameRate = hw->maxFramerate;

                Debugger::Log("frameRateNum", m_frameRate);

                nvEncInitializeParams.frameRateNum = m_frameRate;
                settingChanged = true;
            }
            if (settingChanged)
            {
                NV_ENC_RECONFIGURE_PARAMS nvEncReconfigureParams;
                std::memcpy(&nvEncReconfigureParams.reInitEncodeParams, &nvEncInitializeParams, sizeof(nvEncInitializeParams));
                nvEncReconfigureParams.version = NV_ENC_RECONFIGURE_PARAMS_VER;
                errorCode = pNvEncodeAPI->nvEncReconfigureEncoder(pEncoderInterface, &nvEncReconfigureParams);
                checkf(NV_RESULT(errorCode), StringFormat("Failed to reconfigure encoder setting %d %d %d",
                    errorCode, nvEncInitializeParams.frameRateNum, nvEncConfig.rcParams.averageBitRate).c_str());
            }
        }

        void NvEncoder::SetRates(uint32_t bitRate, int64_t frameRate)
        {
            m_frameRate = frameRate;
            m_targetBitrate = bitRate;

            // [autr] begin...

            HWSettings* hw = HWSettings::getPtr();
            hw->minFramerate = (int)frameRate;
            hw->minBitrate = (int)bitRate;

            // [/autr] end.

            isIdrFrame = true;
        }

        bool NvEncoder::CopyBuffer(void* frame)
        {
            const int curFrameNum = GetCurrentFrameCount() % bufferedFrameNum;
            const auto tex = renderTextures[curFrameNum];
            if (tex == nullptr)
                return false;
            m_device->CopyResourceFromNativeV(tex, frame);
            return true;
        }

        //entry for encoding a frame
        bool NvEncoder::EncodeFrame()
        {
            UpdateSettings();
            uint32 bufferIndexToWrite = frameCount % bufferedFrameNum;
            Frame& frame = bufferedFrames[bufferIndexToWrite];
#pragma region configure per-frame encode parameters
            NV_ENC_PIC_PARAMS picParams = { 0 };
            picParams.version = NV_ENC_PIC_PARAMS_VER;
            picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            picParams.inputBuffer = frame.inputFrame.mappedResource;
            picParams.bufferFmt = frame.inputFrame.bufferFormat;
            picParams.inputWidth = nvEncInitializeParams.encodeWidth;
            picParams.inputHeight = nvEncInitializeParams.encodeHeight;
            picParams.outputBitstream = frame.outputFrame;
            picParams.inputTimeStamp = frameCount;
#pragma endregion
#pragma region start encoding
            if (isIdrFrame)
            {
                picParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR; // [autr] fix (no intras)
                isIdrFrame = false;
            }
            errorCode = pNvEncodeAPI->nvEncEncodePicture(pEncoderInterface, &picParams);
            checkf(NV_RESULT(errorCode), StringFormat("Failed to encode frame, error is %d", errorCode).c_str());
#pragma endregion
            ProcessEncodedFrame(frame);
            frameCount++;
            return true;
        }

        //get encoded frame
        void NvEncoder::ProcessEncodedFrame(Frame& frame)
        {
#pragma region retrieve encoded frame from output buffer
            NV_ENC_LOCK_BITSTREAM lockBitStream = { 0 };
            lockBitStream.version = NV_ENC_LOCK_BITSTREAM_VER;
            lockBitStream.outputBitstream = frame.outputFrame;
            lockBitStream.doNotWait = nvEncInitializeParams.enableEncodeAsync;
            errorCode = pNvEncodeAPI->nvEncLockBitstream(pEncoderInterface, &lockBitStream);
            checkf(NV_RESULT(errorCode), StringFormat("Failed to lock bit stream, error is %d", errorCode).c_str());
            if (lockBitStream.bitstreamSizeInBytes)
            {
                frame.encodedFrame.resize(lockBitStream.bitstreamSizeInBytes);
                std::memcpy(frame.encodedFrame.data(), lockBitStream.bitstreamBufferPtr, lockBitStream.bitstreamSizeInBytes);
            }
            errorCode = pNvEncodeAPI->nvEncUnlockBitstream(pEncoderInterface, frame.outputFrame);
            checkf(NV_RESULT(errorCode), StringFormat("Failed to unlock bit stream, error is %d", errorCode).c_str());
#pragma endregion
            const rtc::scoped_refptr<FrameBuffer> buffer =
                new rtc::RefCountedObject<FrameBuffer>(
                    m_width, m_height, frame.encodedFrame, m_encoderId);
            const int64_t timestamp_us = m_clock->TimeInMicroseconds();
            const int64_t now_us = rtc::TimeMicros();
            const int64_t translated_camera_time_us =
                timestamp_aligner_.TranslateTimestamp(
                    timestamp_us,
                    now_us);

            webrtc::VideoFrame::Builder builder =
                webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(buffer)
                .set_timestamp_us(translated_camera_time_us)
                .set_timestamp_rtp(0)
                .set_ntp_time_ms(rtc::TimeMillis());

            CaptureFrame(builder.build());
        }

        NV_ENC_REGISTERED_PTR NvEncoder::RegisterResource(NV_ENC_INPUT_RESOURCE_TYPE inputType, void* buffer)
        {
            NV_ENC_REGISTER_RESOURCE registerResource = { NV_ENC_REGISTER_RESOURCE_VER };
            registerResource.resourceType = inputType;
            registerResource.resourceToRegister = buffer;

            if (!registerResource.resourceToRegister)
                LogPrint("resource is not initialized");
            registerResource.width = m_width;
            registerResource.height = m_height;
            if (inputType != NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY)
            {
                registerResource.pitch = GetWidthInBytes(m_bufferFormat, m_width);
            }
            else {
                registerResource.pitch = m_width;
            }
            registerResource.bufferFormat = m_bufferFormat;
            registerResource.bufferUsage = NV_ENC_INPUT_IMAGE;
            errorCode = pNvEncodeAPI->nvEncRegisterResource(pEncoderInterface, &registerResource);
            checkf(NV_RESULT(errorCode), StringFormat("nvEncRegisterResource error is %d", errorCode).c_str());
            return registerResource.registeredResource;
        }
        void NvEncoder::MapResources(InputFrame& inputFrame)
        {
            NV_ENC_MAP_INPUT_RESOURCE mapInputResource = { 0 };
            mapInputResource.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
            mapInputResource.registeredResource = inputFrame.registeredResource;
            errorCode = pNvEncodeAPI->nvEncMapInputResource(pEncoderInterface, &mapInputResource);
            checkf(NV_RESULT(errorCode), StringFormat("nvEncMapInputResource error is %d", errorCode).c_str());
            inputFrame.mappedResource = mapInputResource.mappedResource;
        }
        NV_ENC_OUTPUT_PTR NvEncoder::InitializeBitstreamBuffer()
        {
            NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuffer = { 0 };
            createBitstreamBuffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            errorCode = pNvEncodeAPI->nvEncCreateBitstreamBuffer(pEncoderInterface, &createBitstreamBuffer);
            checkf(NV_RESULT(errorCode), StringFormat("nvEncCreateBitstreamBuffer error is %d", errorCode).c_str());
            return createBitstreamBuffer.bitstreamBuffer;
        }
        void NvEncoder::InitEncoderResources()
        {
            for (uint32 i = 0; i < bufferedFrameNum; i++)
            {
                renderTextures[i] = m_device->CreateDefaultTextureV(m_width, m_height);
                void* buffer = AllocateInputResourceV(renderTextures[i]);

                Frame& frame = bufferedFrames[i];
                frame.inputFrame.registeredResource = RegisterResource(m_inputType, buffer);
                frame.inputFrame.bufferFormat = m_bufferFormat;
                MapResources(frame.inputFrame);
                frame.outputFrame = InitializeBitstreamBuffer();
            }
        }

        void NvEncoder::ReleaseFrameInputBuffer(Frame& frame)
        {
            if (frame.inputFrame.mappedResource)
            {
                errorCode = pNvEncodeAPI->nvEncUnmapInputResource(pEncoderInterface, frame.inputFrame.mappedResource);
                checkf(NV_RESULT(errorCode), StringFormat("Failed to unmap input resource %d", errorCode).c_str());
                frame.inputFrame.mappedResource = nullptr;
            }

            if (frame.inputFrame.registeredResource)
            {
                errorCode = pNvEncodeAPI->nvEncUnregisterResource(pEncoderInterface, frame.inputFrame.registeredResource);
                checkf(NV_RESULT(errorCode), StringFormat("Failed to unregister input buffer resource %d", errorCode).c_str());
                frame.inputFrame.registeredResource = nullptr;
            }
        }
        void NvEncoder::ReleaseEncoderResources()
        {
            for (Frame& frame : bufferedFrames)
            {
                ReleaseFrameInputBuffer(frame);
                if (frame.outputFrame != nullptr)
                {
                    errorCode = pNvEncodeAPI->nvEncDestroyBitstreamBuffer(pEncoderInterface, frame.outputFrame);
                    checkf(NV_RESULT(errorCode), StringFormat("Failed to destroy output buffer bit stream %d", errorCode).c_str());
                    frame.outputFrame = nullptr;
                }
            }
        }
        uint32_t NvEncoder::GetNumChromaPlanes(const NV_ENC_BUFFER_FORMAT bufferFormat)
        {
            switch (bufferFormat)
            {
            case NV_ENC_BUFFER_FORMAT_NV12:
            case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
                return 1;
            case NV_ENC_BUFFER_FORMAT_YV12:
            case NV_ENC_BUFFER_FORMAT_IYUV:
            case NV_ENC_BUFFER_FORMAT_YUV444:
            case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
                return 2;
            case NV_ENC_BUFFER_FORMAT_ARGB:
            case NV_ENC_BUFFER_FORMAT_ARGB10:
            case NV_ENC_BUFFER_FORMAT_AYUV:
            case NV_ENC_BUFFER_FORMAT_ABGR:
            case NV_ENC_BUFFER_FORMAT_ABGR10:
                return 0;
            default:
                return -1;
            }
        }
        uint32_t NvEncoder::GetChromaHeight(const NV_ENC_BUFFER_FORMAT bufferFormat, const uint32_t lumaHeight)
        {
            switch (bufferFormat)
            {
            case NV_ENC_BUFFER_FORMAT_YV12:
            case NV_ENC_BUFFER_FORMAT_IYUV:
            case NV_ENC_BUFFER_FORMAT_NV12:
            case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
                return (lumaHeight + 1) / 2;
            case NV_ENC_BUFFER_FORMAT_YUV444:
            case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
                return lumaHeight;
            case NV_ENC_BUFFER_FORMAT_ARGB:
            case NV_ENC_BUFFER_FORMAT_ARGB10:
            case NV_ENC_BUFFER_FORMAT_AYUV:
            case NV_ENC_BUFFER_FORMAT_ABGR:
            case NV_ENC_BUFFER_FORMAT_ABGR10:
                return 0;
            default:
                return 0;
            }
        }
        uint32_t NvEncoder::GetWidthInBytes(const NV_ENC_BUFFER_FORMAT bufferFormat, const uint32_t width)
        {
            switch (bufferFormat) {
            case NV_ENC_BUFFER_FORMAT_NV12:
            case NV_ENC_BUFFER_FORMAT_YV12:
            case NV_ENC_BUFFER_FORMAT_IYUV:
            case NV_ENC_BUFFER_FORMAT_YUV444:
                return width;
            case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
            case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
                return width * 2;
            case NV_ENC_BUFFER_FORMAT_ARGB:
            case NV_ENC_BUFFER_FORMAT_ARGB10:
            case NV_ENC_BUFFER_FORMAT_AYUV:
            case NV_ENC_BUFFER_FORMAT_ABGR:
            case NV_ENC_BUFFER_FORMAT_ABGR10:
                return width * 4;
            default:
                return 0;
            }
        }

        // [autr] begin...

        int NvEncoder::GetRateControlString(const NV_ENC_PARAMS_RC_MODE mode)
        {
            switch (mode) {
            case NV_ENC_PARAMS_RC_CBR:
                return 0;
            case NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ:
                return 1;
            case NV_ENC_PARAMS_RC_CONSTQP:
                return 2;
            case NV_ENC_PARAMS_RC_CBR_HQ:
                return 3;
            case NV_ENC_PARAMS_RC_VBR:
                return 4;
            default:
                return 0;
            }
        }
        NV_ENC_PARAMS_RC_MODE NvEncoder::GetRateControlMode(int mode)
        {

            if (mode == 0) return NV_ENC_PARAMS_RC_CBR; // only minimum / average bitrate is used (no maxbitrate)
            if (mode == 1 || mode == 10) return NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ; // ?
            if (mode == 2 || mode == 20) return NV_ENC_PARAMS_RC_CONSTQP; // no min/max/average bitrate - only minQP and maxQP used
            if (mode == 3 || mode == 30) return NV_ENC_PARAMS_RC_CBR_HQ; // ?
            if (mode == 4 || mode == 40) return NV_ENC_PARAMS_RC_VBR; // uses minimum AND maximum bitrate
            return NV_ENC_PARAMS_RC_CBR;
        }

        // [/autr] end.

    } // end namespace webrtc
} // end namespace unity
