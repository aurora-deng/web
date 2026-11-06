# STATUS: DRAFT — container image build and startup are not yet verified.
# Transport demo only. For the PostgreSQL workflow use scripts/project/reproduce.py.
FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build python3 libssl-dev libnghttp2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json main.cpp ./
COPY backend ./backend
COPY client ./client
COPY frontend ./frontend
COPY scripts/project ./scripts/project
COPY static ./static
RUN cmake --preset docker && cmake --build --preset docker -j2

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl libssl3t64 libnghttp2-14 libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --shell /usr/sbin/nologin nebula
WORKDIR /app
COPY --from=builder /src/build/docker/webserver /usr/local/bin/webserver
COPY --from=builder /src/frontend ./frontend
COPY --from=builder /src/static ./static
USER 10001
ENV WEB_SERVER_PORT=8080
EXPOSE 8080
HEALTHCHECK --interval=15s --timeout=3s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${WEB_SERVER_PORT}/health/live" || exit 1
ENTRYPOINT ["/usr/local/bin/webserver"]
