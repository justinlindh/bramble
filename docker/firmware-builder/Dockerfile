# Firmware CI builder image for Bramble idf-node runners.
#
# Pinning strategy:
# - ESP-IDF/toolchain pin: espressif/idf:v5.4.1
# - uv pin: 0.6.3
#
# This image is intended to be published as bramble/idf-node:v5.4.1 and consumed
# by the idf-node runner label mapping in runner config.
FROM espressif/idf:v5.4.1

ARG UV_VERSION=0.6.3

WORKDIR /workspace

# CI baseline utilities that firmware workflows use directly.
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    ca-certificates \
    curl \
    git \
    jq \
    make \
    rsync \
    unzip \
  && rm -rf /var/lib/apt/lists/*

# Install pinned uv binary globally.
RUN curl -LsSf https://astral.sh/uv/${UV_VERSION}/install.sh | sh \
  && mv /root/.local/bin/uv /usr/local/bin/uv \
  && mv /root/.local/bin/uvx /usr/local/bin/uvx \
  && uv --version \
  && uvx --version

# Basic runtime defaults for CI jobs.
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8
