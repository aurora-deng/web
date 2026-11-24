# Build context is staged from an already verified Linux archive by offline_docker.py.
# The web and fleet targets use read-only runtime mounts. Bundled targets stage the checked runtime.
FROM scratch AS web
ARG NEBULA_REVISION
LABEL org.opencontainers.image.source="https://github.com/aurora-deng/web" \
      org.opencontainers.image.revision="${NEBULA_REVISION}" \
      org.opencontainers.image.title="NEBULA Web"
WORKDIR /app
COPY app/ /app/
USER 10001:10001
ENV WEB_SERVER_PORT=8080
EXPOSE 8080
ENTRYPOINT ["/runtime/ld-linux-x86-64.so.2", "--library-path", "/runtime", "/app/webserver"]

FROM scratch AS fleet
ARG NEBULA_REVISION
LABEL org.opencontainers.image.source="https://github.com/aurora-deng/web" \
      org.opencontainers.image.revision="${NEBULA_REVISION}" \
      org.opencontainers.image.title="NEBULA C++ Fleet"
WORKDIR /state
COPY fleet/ /app/
USER 10001:10001
ENTRYPOINT ["/runtime/ld-linux-x86-64.so.2", "--library-path", "/runtime"]
CMD ["/app/nebula_fleet_ctl"]

FROM web AS web-bundled
COPY runtime-web/ /runtime/

FROM fleet AS fleet-bundled
COPY runtime-fleet/ /runtime/
