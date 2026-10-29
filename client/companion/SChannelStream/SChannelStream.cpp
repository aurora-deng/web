#define SCHANNEL_USE_BLACKLISTS
#include "SChannelStream.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include "client/companion/BoundedPool/BoundedPool.h"
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef NEBULA_TLS_DIAGNOSTICS
#include <cstdio>
#endif
namespace nebula::client {
namespace net=boost::asio;using tcp=net::ip::tcp;
namespace {
std::wstring wide(const std::string&value){const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(!n)throw std::runtime_error("Invalid TLS server name");std::wstring result(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),n);return result;}
void status(SECURITY_STATUS value,const char*message){if(value!=SEC_E_OK)throw std::runtime_error(message);}
}
struct SChannelStream::Impl {
    tcp::socket socket;ServerTrust trust;CredHandle credentials{};CtxtHandle context{};
    bool hasCredentials{},hasContext{},validated{};
    SecPkgContext_StreamSizes sizes{};std::vector<unsigned char>encrypted,plain;
    std::size_t plainOffset{};std::mutex reader,writer;
    mutable std::mutex failureMutex;std::string failure;
    BoundedPool*reads{};BoundedPool*writes{};
    Impl(net::io_context&io,ServerTrust t):socket(io),trust(std::move(t)){}
    ~Impl(){boost::system::error_code ignored;socket.close(ignored);if(hasContext)DeleteSecurityContext(&context);if(hasCredentials)FreeCredentialsHandle(&credentials);}
    void receive(){if(encrypted.size()>=256*1024)throw std::runtime_error("TLS input buffer limit");std::array<unsigned char,16384>buffer;const auto n=socket.read_some(net::buffer(buffer));encrypted.insert(encrypted.end(),buffer.begin(),buffer.begin()+n);}
    void verify(){
        PCCERT_CONTEXT certificate{};status(QueryContextAttributesW(&context,SECPKG_ATTR_REMOTE_CERT_CONTEXT,&certificate),"TLS peer certificate missing");
        struct CertificateGuard{PCCERT_CONTEXT value;~CertificateGuard(){CertFreeCertificateContext(value);}}guard{certificate};
        if(CertVerifyTimeValidity(nullptr,certificate->pCertInfo)!=0)throw std::runtime_error("TLS certificate expired or not yet valid");
        if(trust.certificateSha256.empty())return; // SChannel 已执行系统信任链和名称校验。
        const auto digest=sha256(std::string_view(reinterpret_cast<const char*>(certificate->pbCertEncoded),certificate->cbCertEncoded));
        if(digest!=trust.certificateSha256)throw std::runtime_error("TLS certificate fingerprint changed; confirmation required");
        // 根证书只存在内存中。匹配指纹不等于跳过名称/用途/有效期校验。
        HCERTSTORE roots=CertOpenStore(CERT_STORE_PROV_MEMORY,0,0,CERT_STORE_CREATE_NEW_FLAG,nullptr);
        if(!roots)throw std::runtime_error("Cannot create process-local certificate store");
        struct StoreGuard{HCERTSTORE value;~StoreGuard(){CertCloseStore(value,0);}}storeGuard{roots};
        if(!CertAddCertificateContextToStore(roots,certificate,CERT_STORE_ADD_ALWAYS,nullptr))throw std::runtime_error("Cannot prepare process-local certificate trust");
        CERT_CHAIN_ENGINE_CONFIG config{};config.cbSize=sizeof(config);config.hExclusiveRoot=roots;
        HCERTCHAINENGINE engine{};if(!CertCreateCertificateChainEngine(&config,&engine))throw std::runtime_error("Cannot create process-local certificate verifier");
        struct EngineGuard{HCERTCHAINENGINE value;~EngineGuard(){CertFreeCertificateChainEngine(value);}}engineGuard{engine};
        CERT_CHAIN_PARA parameters{};parameters.cbSize=sizeof(parameters);LPSTR usage=const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        parameters.RequestedUsage.dwType=USAGE_MATCH_TYPE_AND;parameters.RequestedUsage.Usage={1,&usage};
        PCCERT_CHAIN_CONTEXT chain{};
        if(!CertGetCertificateChain(engine,certificate,nullptr,certificate->hCertStore,&parameters,CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL|CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,nullptr,&chain))throw std::runtime_error("TLS certificate chain verification failed");
        struct ChainGuard{PCCERT_CHAIN_CONTEXT value;~ChainGuard(){CertFreeCertificateChain(value);}}chainGuard{chain};
        auto server=wide(trust.host);SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra{};extra.cbSize=sizeof(extra);extra.dwAuthType=AUTHTYPE_SERVER;extra.pwszServerName=server.data();
        CERT_CHAIN_POLICY_PARA policy{};policy.cbSize=sizeof(policy);policy.pvExtraPolicyPara=&extra;
        CERT_CHAIN_POLICY_STATUS result{};result.cbSize=sizeof(result);
        if(!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,chain,&policy,&result)||result.dwError)
            throw std::runtime_error("TLS certificate name, purpose or trust policy failed: "+std::to_string(result.dwError));
    }
    void handshake(SecBufferDesc*pending=nullptr){
        // SCHANNEL_CRED 是旧接口，不会协商TLS1.3；Windows11使用新凭据结构。
        TLS_PARAMETERS protocols{};protocols.grbitDisabledProtocols=SP_PROT_SSL2_CLIENT|SP_PROT_SSL3_CLIENT|SP_PROT_TLS1_0_CLIENT|SP_PROT_TLS1_1_CLIENT|SP_PROT_TLS1_2_CLIENT;
        SCH_CREDENTIALS credential{};credential.dwVersion=SCH_CREDENTIALS_VERSION;
        credential.cTlsParameters=1;credential.pTlsParameters=&protocols;
        credential.dwFlags=SCH_CRED_NO_DEFAULT_CREDS|(trust.certificateSha256.empty()?SCH_CRED_AUTO_CRED_VALIDATION:SCH_CRED_MANUAL_CRED_VALIDATION);
        if(!hasCredentials){status(AcquireCredentialsHandleW(nullptr,const_cast<wchar_t*>(UNISP_NAME_W),SECPKG_CRED_OUTBOUND,nullptr,&credential,nullptr,nullptr,&credentials,nullptr),"Cannot acquire Windows TLS1.3 credentials");hasCredentials=true;}
        const auto flags=ISC_REQ_SEQUENCE_DETECT|ISC_REQ_REPLAY_DETECT|ISC_REQ_CONFIDENTIALITY|ISC_REQ_ALLOCATE_MEMORY|ISC_REQ_STREAM|ISC_REQ_EXTENDED_ERROR|
            (trust.certificateSha256.empty()?0:ISC_REQ_MANUAL_CRED_VALIDATION);
        auto name=wide(trust.host);bool first=!hasContext;
        alignas(SEC_APPLICATION_PROTOCOLS) std::array<unsigned char,offsetof(SEC_APPLICATION_PROTOCOLS,ProtocolLists)+offsetof(SEC_APPLICATION_PROTOCOL_LIST,ProtocolList)+9>alpnBytes{};
        auto*alpn=reinterpret_cast<SEC_APPLICATION_PROTOCOLS*>(alpnBytes.data());alpn->ProtocolListsSize=static_cast<ULONG>(alpnBytes.size()-offsetof(SEC_APPLICATION_PROTOCOLS,ProtocolLists));
        alpn->ProtocolLists[0].ProtoNegoExt=SecApplicationProtocolNegotiationExt_ALPN;alpn->ProtocolLists[0].ProtocolListSize=9;alpn->ProtocolLists[0].ProtocolList[0]=8;std::memcpy(alpn->ProtocolLists[0].ProtocolList+1,"http/1.1",8);
        SecBuffer alpnBuffer{static_cast<ULONG>(alpnBytes.size()),SECBUFFER_APPLICATION_PROTOCOLS,alpnBytes.data()};SecBufferDesc alpnInput{SECBUFFER_VERSION,1,&alpnBuffer};
        for(unsigned round=0;round<100;round++){
            SecBuffer input[2]{{static_cast<ULONG>(encrypted.size()),SECBUFFER_TOKEN,encrypted.data()},{0,SECBUFFER_EMPTY,nullptr}};
            SecBufferDesc in{SECBUFFER_VERSION,2,input};SecBuffer output{0,SECBUFFER_TOKEN,nullptr};SecBufferDesc out{SECBUFFER_VERSION,1,&output};ULONG attributes{};
            auto*incoming=pending?pending:&in;
            const auto code=InitializeSecurityContextW(&credentials,first?nullptr:&context,name.data(),flags,0,SECURITY_NATIVE_DREP,first?&alpnInput:incoming,0,&context,&out,&attributes,nullptr);
#ifdef NEBULA_TLS_DIAGNOSTICS
            std::fprintf(stderr,"ISC code=%lx input=%zu out=%lu first=%d\n",static_cast<unsigned long>(code),encrypted.size(),output.cbBuffer,first);
#endif
            if(first&&(code==SEC_E_OK||code==SEC_I_CONTINUE_NEEDED))hasContext=true;first=false;
            if(output.pvBuffer){try{if(output.cbBuffer)net::write(socket,net::buffer(output.pvBuffer,output.cbBuffer));}catch(...){FreeContextBuffer(output.pvBuffer);throw;}FreeContextBuffer(output.pvBuffer);}
            if(code==SEC_E_INCOMPLETE_MESSAGE){receive();continue;}
            if(code!=SEC_E_OK&&code!=SEC_I_CONTINUE_NEEDED)throw std::runtime_error("Windows TLS handshake failed: "+std::to_string(static_cast<unsigned long>(code)));
            ULONG extra=0;for(ULONG index=0;index<incoming->cBuffers;index++)if(incoming->pBuffers[index].BufferType==SECBUFFER_EXTRA)extra=incoming->pBuffers[index].cbBuffer;
#ifdef NEBULA_TLS_DIAGNOSTICS
            std::fprintf(stderr,"ISC extra=%lu\n",extra);
#endif
            pending=nullptr;
            if(extra)encrypted.erase(encrypted.begin(),encrypted.end()-extra);else encrypted.clear();
            if(code==SEC_E_OK){verify();status(QueryContextAttributesW(&context,SECPKG_ATTR_STREAM_SIZES,&sizes),"TLS stream sizes unavailable");validated=true;return;}
            if(encrypted.empty())receive();
        }throw std::runtime_error("TLS handshake round limit");
    }
};
SChannelStream::SChannelStream(net::io_context&io,ServerTrust trust):impl_(std::make_unique<Impl>(io,std::move(trust))){}
SChannelStream::~SChannelStream()=default;
SChannelStream::executor_type SChannelStream::get_executor()noexcept{return impl_->socket.get_executor();}
tcp::socket&SChannelStream::socket()noexcept{return impl_->socket;}
void SChannelStream::setAsyncPools(BoundedPool&reads,BoundedPool&writes){impl_->reads=&reads;impl_->writes=&writes;}
bool SChannelStream::schedule(bool write,std::function<void()>operation){auto*pool=write?impl_->writes:impl_->reads;return pool&&pool->submit(std::move(operation));}
void SChannelStream::connect(){
    auto&i=*impl_;tcp::resolver resolver(i.socket.get_executor());auto endpoints=resolver.resolve(i.trust.connectHost.empty()?i.trust.host:i.trust.connectHost,std::to_string(i.trust.port));
    boost::system::error_code last=net::error::host_not_found;
    for(const auto&endpoint:endpoints){
        boost::system::error_code ignored;i.socket.close(ignored);i.socket.open(endpoint.endpoint().protocol());i.socket.non_blocking(true);i.socket.connect(endpoint,last);
        if(last==net::error::would_block||last==net::error::in_progress){WSAPOLLFD poll{i.socket.native_handle(),POLLOUT,0};
            if(WSAPoll(&poll,1,5000)>0){int result=0;int size=sizeof(result);if(getsockopt(i.socket.native_handle(),SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&result),&size)==0)last=boost::system::error_code(result,boost::system::system_category());}
            else last=net::error::timed_out;}
        if(!last){i.socket.non_blocking(false);break;}
    }
    if(last)throw boost::system::system_error(last);
    // TLS 头和正文可能拆成小记录，禁用 Nagle，避免等待对端延迟 ACK。
    i.socket.set_option(tcp::no_delay(true));
    DWORD timeout=static_cast<DWORD>(std::clamp<std::int64_t>(i.trust.readTimeout.count(),1000,60000));
    setsockopt(i.socket.native_handle(),SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    setsockopt(i.socket.native_handle(),SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    i.handshake();
}
void SChannelStream::close()noexcept{boost::system::error_code ignored;impl_->socket.shutdown(tcp::socket::shutdown_both,ignored);impl_->socket.close(ignored);}
std::string SChannelStream::lastFailure()const{std::lock_guard lock(impl_->failureMutex);return impl_->failure;}
std::size_t SChannelStream::read(void*data,std::size_t length,boost::system::error_code&ec){
    auto&i=*impl_;std::lock_guard lock(i.reader);ec.clear();
    try{if(!i.validated)throw std::runtime_error("TLS not validated");
        for(;;){if(i.plainOffset<i.plain.size()){const auto n=std::min(length,i.plain.size()-i.plainOffset);std::memcpy(data,i.plain.data()+i.plainOffset,n);i.plainOffset+=n;return n;}
            i.plain.clear();i.plainOffset=0;if(i.encrypted.empty())i.receive();
            SecBuffer buffers[4]{{static_cast<ULONG>(i.encrypted.size()),SECBUFFER_DATA,i.encrypted.data()},{0,SECBUFFER_EMPTY,nullptr},{0,SECBUFFER_EMPTY,nullptr},{0,SECBUFFER_EMPTY,nullptr}};
            SecBufferDesc descriptor{SECBUFFER_VERSION,4,buffers};const auto code=DecryptMessage(&i.context,&descriptor,0,nullptr);
#ifdef NEBULA_TLS_DIAGNOSTICS
            std::fprintf(stderr,"DEC code=%lx length=%zu types=%lu/%lu/%lu/%lu sizes=%lu/%lu/%lu/%lu\n",static_cast<unsigned long>(code),i.encrypted.size(),buffers[0].BufferType,buffers[1].BufferType,buffers[2].BufferType,buffers[3].BufferType,buffers[0].cbBuffer,buffers[1].cbBuffer,buffers[2].cbBuffer,buffers[3].cbBuffer);
#endif
            if(code==SEC_E_INCOMPLETE_MESSAGE){i.receive();continue;}if(code==SEC_I_CONTEXT_EXPIRED){ec=net::error::eof;return 0;}
            if(code==SEC_I_RENEGOTIATE){
                // TLS1.3票据返回的DATA可能长为0，实际控制记录在EXTRA；不能把空DATA
                // 当成完成的握手，否则下一次Decrypt会用新序号重复解密同一记录。
                for(const auto&b:buffers)if(b.BufferType==SECBUFFER_EXTRA&&b.cbBuffer){const auto*p=static_cast<const unsigned char*>(b.pvBuffer);std::vector<unsigned char>pending(p,p+b.cbBuffer);i.encrypted=std::move(pending);break;}
                // TLS1.3票据/密钥更新也走SSPI状态机；握手写入与应用写入互斥。
                std::lock_guard writeLock(i.writer);i.handshake();continue;
            }
            if(code!=SEC_E_OK)throw std::runtime_error("TLS decryption or renegotiation failed: "+std::to_string(static_cast<unsigned long>(code)));
            ULONG extra=0;for(const auto&buffer:buffers){if(buffer.BufferType==SECBUFFER_DATA&&buffer.cbBuffer){auto*p=static_cast<unsigned char*>(buffer.pvBuffer);i.plain.insert(i.plain.end(),p,p+buffer.cbBuffer);}if(buffer.BufferType==SECBUFFER_EXTRA)extra=buffer.cbBuffer;}
            if(extra)i.encrypted.erase(i.encrypted.begin(),i.encrypted.end()-extra);else i.encrypted.clear();
        }
    }catch(const boost::system::system_error&error){ec=error.code();}catch(const std::exception&error){{std::lock_guard lock(i.failureMutex);i.failure=error.what();}ec=make_error_code(boost::system::errc::protocol_error);}return 0;
}
std::size_t SChannelStream::write(const void*data,std::size_t length,boost::system::error_code&ec){
    auto&i=*impl_;std::lock_guard lock(i.writer);ec.clear();
    try{if(!i.validated)throw std::runtime_error("Application data blocked until TLS validation");
        const auto n=std::min(length,static_cast<std::size_t>(i.sizes.cbMaximumMessage));std::vector<unsigned char>encoded(i.sizes.cbHeader+n+i.sizes.cbTrailer);
        std::memcpy(encoded.data()+i.sizes.cbHeader,data,n);
        SecBuffer buffers[4]{{i.sizes.cbHeader,SECBUFFER_STREAM_HEADER,encoded.data()},{static_cast<ULONG>(n),SECBUFFER_DATA,encoded.data()+i.sizes.cbHeader},{i.sizes.cbTrailer,SECBUFFER_STREAM_TRAILER,encoded.data()+i.sizes.cbHeader+n},{0,SECBUFFER_EMPTY,nullptr}};
        SecBufferDesc descriptor{SECBUFFER_VERSION,4,buffers};status(EncryptMessage(&i.context,0,&descriptor,0),"TLS encryption failed");
        const std::array<net::const_buffer,3>output{net::buffer(buffers[0].pvBuffer,buffers[0].cbBuffer),net::buffer(buffers[1].pvBuffer,buffers[1].cbBuffer),net::buffer(buffers[2].pvBuffer,buffers[2].cbBuffer)};
        net::write(i.socket,output,ec);return ec?0:n;
    }catch(const boost::system::system_error&error){ec=error.code();}catch(...){ec=make_error_code(boost::system::errc::protocol_error);}return 0;
}
void teardown(boost::beast::role_type,SChannelStream&stream,boost::system::error_code&ec){stream.close();ec.clear();}
}
