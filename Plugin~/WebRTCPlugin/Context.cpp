#include "pch.h"
#include "WebRTCPlugin.h"
#include "Context.h"
#include "DummyVideoEncoder.h"
#include "VideoCaptureTrackSource.h"
#include "MediaStreamObserver.h"
#include "SetSessionDescriptionObserver.h"

namespace WebRTC
{
    ContextManager ContextManager::s_instance;

    Context* ContextManager::GetContext(int uid) const
    {
        auto it = s_instance.m_contexts.find(uid);
        if (it != s_instance.m_contexts.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    Context* ContextManager::CreateContext(int uid, UnityEncoderType encoderType)
    {
        auto it = s_instance.m_contexts.find(uid);
        if (it != s_instance.m_contexts.end()) {
            DebugLog("Using already created context with ID %d", uid);
            return nullptr;
        }
        auto ctx = new Context(uid, encoderType);
        s_instance.m_contexts[uid].reset(ctx);
        return ctx;
    }

    void ContextManager::SetCurContext(Context* context)
    {
        curContext = context;
    }

    void ContextManager::DestroyContext(int uid)
    {
        auto it = s_instance.m_contexts.find(uid);
        if (it != s_instance.m_contexts.end()) {
            s_instance.m_contexts.erase(it);
            DebugLog("Unregistered context with ID %d", uid);
        }
    }

    ContextManager::~ContextManager()
    {
        if (m_contexts.size()) {
            DebugWarning("%lu remaining context(s) registered", m_contexts.size());
        }
        m_contexts.clear();
    }

    bool Convert(const std::string& str, webrtc::PeerConnectionInterface::RTCConfiguration& config)
    {
        config = webrtc::PeerConnectionInterface::RTCConfiguration{};
        Json::CharReaderBuilder builder;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value configJson;
        Json::String err;
        auto ok = reader->parse(str.c_str(), str.c_str() + static_cast<int>(str.length()), &configJson, &err);
        if (!ok)
        {
            //json parse failed.
            return false;
        }

        Json::Value iceServersJson = configJson["iceServers"];
        if (!iceServersJson)
            return false;
        for (auto iceServerJson : iceServersJson)
        {
            webrtc::PeerConnectionInterface::IceServer iceServer;
            for (auto url : iceServerJson["urls"])
            {
                iceServer.urls.push_back(url.asString());
            }
            if (!iceServerJson["username"].isNull())
            {
                iceServer.username = iceServerJson["username"].asString();
            }
            if (!iceServerJson["username"].isNull())
            {
                iceServer.password = iceServerJson["credential"].asString();
            }
            config.servers.push_back(iceServer);
        }
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        return true;
    }
#pragma warning(push)
#pragma warning(disable: 4715)
    webrtc::SdpType ConvertSdpType(RTCSdpType type)
    {
        switch (type)
        {
        case RTCSdpType::Offer:
            return webrtc::SdpType::kOffer;
        case RTCSdpType::PrAnswer:
            return webrtc::SdpType::kPrAnswer;
        case RTCSdpType::Answer:
            return webrtc::SdpType::kAnswer;
        }
        throw std::invalid_argument("Unknown RTCSdpType");
    }

    RTCSdpType ConvertSdpType(webrtc::SdpType type)
    {
        switch (type)
        {
        case webrtc::SdpType::kOffer:
            return RTCSdpType::Offer;
        case webrtc::SdpType::kPrAnswer:
            return RTCSdpType::PrAnswer;
        case webrtc::SdpType::kAnswer:
            return RTCSdpType::Answer;
        default:
            throw std::invalid_argument("Unknown SdpType");
        }
    }
#pragma warning(pop)

    Context::Context(int uid, UnityEncoderType encoderType)
        : m_uid(uid)
        , m_encoderType(encoderType)
    {
        m_workerThread.reset(new rtc::Thread(rtc::SocketServer::CreateDefault()));
        m_workerThread->Start();
        m_signalingThread.reset(new rtc::Thread(rtc::SocketServer::CreateDefault()));
        m_signalingThread->Start();

        rtc::InitializeSSL();

        m_audioDevice = new rtc::RefCountedObject<DummyAudioDevice>();

#if defined(SUPPORT_METAL) && defined(SUPPORT_SOFTWARE_ENCODER)
        //Always use SoftwareEncoder on Mac for now.
        std::unique_ptr<webrtc::VideoEncoderFactory> videoEncoderFactory = webrtc::CreateBuiltinVideoEncoderFactory();
#else
        std::unique_ptr<webrtc::VideoEncoderFactory> videoEncoderFactory =
            m_encoderType == UnityEncoderType::UnityEncoderHardware ?
            std::make_unique<DummyVideoEncoderFactory>() : webrtc::CreateBuiltinVideoEncoderFactory();
#endif

        m_peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
                                m_workerThread.get(),
                                m_workerThread.get(),
                                m_signalingThread.get(),
                                m_audioDevice,
                                webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>(),
                                webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>(),
                                std::move(videoEncoderFactory),
                                webrtc::CreateBuiltinVideoDecoderFactory(),
                                nullptr,
                                nullptr);
    }

    Context::~Context()
    {
        m_peerConnectionFactory = nullptr;
        m_audioTrack = nullptr;

        m_mediaSteamTrackList.clear();
        m_mapClients.clear();
        m_mapVideoCapturer.clear();
        m_mapMediaStream.clear();
        m_mapMediaStreamObserver.clear();
        m_mapSetSessionDescriptionObserver.clear();
        m_mapStatsCollectorCallback.clear();
        m_mapVideoEncoderParameter.clear();
        m_mapDataChannels.clear();

        m_workerThread->Quit();
        m_workerThread.reset();
        m_signalingThread->Quit();
        m_signalingThread.reset();
    }


    bool Context::InitializeEncoder(IEncoder* encoder, webrtc::MediaStreamTrackInterface* track)
    {
        m_mapVideoCapturer[track]->SetEncoder(encoder);
        m_mapVideoCapturer[track]->StartEncoder();
        return true;
    }

    void Context::EncodeFrame(webrtc::MediaStreamTrackInterface* track)
    {
        if (!m_mapVideoCapturer[track]->EncodeVideoData())
        {
        }
    }

    const VideoEncoderParameter* Context::GetEncoderParameter(const webrtc::MediaStreamTrackInterface* track) {
        return m_mapVideoEncoderParameter[track].get();
    }

    void Context::SetEncoderParameter(const webrtc::MediaStreamTrackInterface* track, int width, int height, UnityEncoderType type) {
        m_mapVideoEncoderParameter[track] = std::make_unique<VideoEncoderParameter>(width, height, type);
    }

    UnityEncoderType Context::GetEncoderType() const
    {
        return m_encoderType;
    }

    webrtc::MediaStreamInterface* Context::CreateMediaStream(const std::string& streamId)
    {
        rtc::scoped_refptr<webrtc::MediaStreamInterface> stream =
            m_peerConnectionFactory->CreateLocalMediaStream(streamId);
        m_mapMediaStreamObserver[stream] = std::make_unique<MediaStreamObserver>(stream);
        stream->RegisterObserver(m_mapMediaStreamObserver[stream].get());
        return stream.release();
    }

    void Context::DeleteMediaStream(webrtc::MediaStreamInterface* stream)
    {
        stream->UnregisterObserver(m_mapMediaStreamObserver[stream].get());
        m_mapMediaStreamObserver.erase(stream);
        stream->Release();
    }

    MediaStreamObserver* Context::GetObserver(const webrtc::MediaStreamInterface* stream)
    {
        return m_mapMediaStreamObserver[stream].get();
    }

    webrtc::VideoTrackInterface* Context::CreateVideoTrack(const std::string& label, void* frameBuffer, int32 width, int32 height, int32 bitRate)
    {
        auto videoCapturer = std::make_unique<NvVideoCapturer>();
        const std::unique_ptr<NvVideoCapturer>::pointer ptr = videoCapturer.get();
        videoCapturer->SetFrameBuffer(frameBuffer);

        const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source(
            WebRTC::VideoCapturerTrackSource::Create(m_workerThread.get(), std::move(videoCapturer), false));
        webrtc::VideoTrackInterface* videoTrack =
            m_peerConnectionFactory->CreateVideoTrack(label, source).release();
        m_mapVideoCapturer[videoTrack] = ptr;
        return videoTrack;
    }

    void Context::StopMediaStreamTrack(webrtc::MediaStreamTrackInterface* track)
    {
        if(m_mapVideoCapturer.count(track) > 0)
        {
            m_mapVideoCapturer[track]->Stop();
        }
    }

    webrtc::AudioTrackInterface* Context::CreateAudioTrack(const std::string& label)
    {
        //avoid optimization specially for voice
        cricket::AudioOptions audioOptions;
        audioOptions.auto_gain_control = false;
        audioOptions.noise_suppression = false;
        audioOptions.highpass_filter = false;
        return m_peerConnectionFactory->CreateAudioTrack(label, m_peerConnectionFactory->CreateAudioSource(audioOptions)).release();
    }

    void Context::DeleteMediaStreamTrack(webrtc::MediaStreamTrackInterface* track)
    {
        track->Release();
    }

    void Context::ProcessAudioData(const float* data, int32 size)
    {
        m_audioDevice->ProcessAudioData(data, size);
    }

    DataChannelObject* Context::CreateDataChannel(PeerConnectionObject* obj, const char* label, const RTCDataChannelInit& options)
    {
        webrtc::DataChannelInit config;
        config.reliable = options.reliable;
        config.ordered = options.ordered;
        config.maxRetransmitTime = options.maxRetransmitTime;
        config.maxRetransmits = options.maxRetransmits;
        config.protocol = options.protocol;
        config.negotiated = options.negotiated;

        auto channel = obj->connection->CreateDataChannel(label, &config);
        auto dataChannelObj = std::make_unique<DataChannelObject>(channel, *obj);
        auto ptr = dataChannelObj.get();
        m_mapDataChannels[ptr] = std::move(dataChannelObj);
        return ptr;
    }

    void Context::AddDataChannel(std::unique_ptr<DataChannelObject>& channel) {
        const auto ptr = channel.get();
        m_mapDataChannels[ptr] = std::move(channel);
    }

    void Context::DeleteDataChannel(DataChannelObject* obj)
    {
        if (m_mapDataChannels.count(obj) > 0)
        {
            m_mapDataChannels.erase(obj);
        }
    }

    void Context::AddObserver(const webrtc::PeerConnectionInterface* connection, const rtc::scoped_refptr<SetSessionDescriptionObserver>& observer)
    {
        m_mapSetSessionDescriptionObserver[connection] = observer;
    }

    void Context::RemoveObserver(const webrtc::PeerConnectionInterface* connection)
    {
        m_mapSetSessionDescriptionObserver.erase(connection);
    }

    SetSessionDescriptionObserver* Context::GetObserver(webrtc::PeerConnectionInterface* connection)
    {
        return m_mapSetSessionDescriptionObserver[connection];
    }
}
