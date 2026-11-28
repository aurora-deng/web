# This application uses OpenSSL in its native TLS transport. A gRPC package
# exporting its own unprefixed SSL/Crypto implementation must not enter the same
# executable: hiding archive exports does not prevent local symbol resolution.
function(webserver_require_compatible_grpc_tls)
    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR "Native TLS requires the selected OpenSSL targets before checking gRPC")
    endif()
    foreach(bundled_target IN ITEMS gRPC::SSL gRPC::Crypto gRPC::ssl gRPC::crypto)
        if(TARGET "${bundled_target}")
            message(FATAL_ERROR
                "Incompatible gRPC TLS provider: ${bundled_target} bundles a second SSL/Crypto implementation. "
                "The native TLS transport already uses OpenSSL; mixing it with bundled BoringSSL can crash at startup. "
                "Rebuild the approved gRPC version with -DgRPC_SSL_PROVIDER=package, using the same OpenSSL headers "
                "and libraries as this application. Do not disable TLS or remove certificate verification.")
        endif()
    endforeach()
endfunction()
