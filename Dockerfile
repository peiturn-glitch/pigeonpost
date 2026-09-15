# syntax=docker/dockerfile:1

# --- Frontend: Vue 3 + Vite ---------------------------------------------------
FROM node:22-alpine AS frontend
WORKDIR /src
COPY frontend/package.json frontend/package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci; else npm install; fi
COPY frontend/ ./
RUN npm run build

# --- Backend: C++20 -----------------------------------------------------------
FROM debian:trixie AS backend
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config ca-certificates curl \
        libssl-dev libcurl4-openssl-dev libsqlite3-dev libgmime-3.0-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
ARG HTTPLIB_VERSION=v0.20.0
RUN mkdir -p /opt/httplib \
    && curl -fsSL -o /opt/httplib/httplib.h \
       https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_VERSION}/httplib.h
WORKDIR /src
COPY backend/ ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHTTPLIB_INCLUDE_DIR=/opt/httplib \
    && cmake --build build -j"$(nproc)"

# --- Runtime ------------------------------------------------------------------
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libssl3t64 libcurl4t64 libsqlite3-0 libgmime-3.0-0t64 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /data brevduva \
    && mkdir -p /data && chown brevduva /data

COPY --from=backend /src/build/brevduva-server /app/brevduva-server
COPY --from=frontend /src/dist /app/public

ENV BREVDUVA_HOST=0.0.0.0 \
    BREVDUVA_PORT=8080 \
    BREVDUVA_DATA_DIR=/data \
    BREVDUVA_STATIC_DIR=/app/public

USER brevduva
VOLUME ["/data"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s CMD ["/app/brevduva-server", "--healthcheck"]
ENTRYPOINT ["/app/brevduva-server"]
