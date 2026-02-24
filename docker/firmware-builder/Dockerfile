# Firmware CI builder image for Bramble.
#
# Pinning strategy (conservative first pass):
# - ESP-IDF/toolchain pin: espressif/idf:v5.4.1
# - Avoid additional apt/pip installs here to keep the toolchain surface stable.
#
# When updating, bump intentionally and re-validate firmware build reproducibility.
FROM espressif/idf:v5.4.1

WORKDIR /workspace

# Basic runtime defaults for CI jobs.
ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8
