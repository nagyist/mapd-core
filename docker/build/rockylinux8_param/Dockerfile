# Build libglvnd for libGL, libEGL, libOpenGL
# Not currently pulled in by nvidia-docker2
ARG BASE_CUDA_IMAGE
FROM ${BASE_CUDA_IMAGE}

ENV NVIDIA_DRIVER_CAPABILITIES compute,utility,graphics

# Add entrypoint script to run ldconfig
RUN echo $'#!/bin/bash\n\
      ldconfig\n\
      exec "$@"'\
    >> /docker-entrypoint.sh && \
    chmod +x /docker-entrypoint.sh
ENTRYPOINT ["/docker-entrypoint.sh"]

RUN dnf groupinstall -y "Development Tools"

RUN dnf install -y \
        zlib-devel \
        epel-release \
        libssh \
        git \
        maven \
        java-1.8.0-openjdk-devel \
        java-1.8.0-openjdk-headless \
        wget \
        curl \
        sudo \
        libX11-devel \
        mesa-libGL-devel \
        environment-modules \
        which \
        valgrind \
        zip unzip && \
    rm -rf /var/cache/dnf/*
RUN dnf install -y \
        cloc \
        jq && \
    rm -rf /var/cache/dnf/*

RUN mkdir -p /etc/vulkan/icd.d && \
    echo '{ "file_format_version" : "1.0.0", "ICD" : { "library_path" : "libGLX_nvidia.so.0", "api_version" : "1.1.99" } }' > /etc/vulkan/icd.d/nvidia_icd.json

RUN echo > /etc/ld.so.preload

# copy the script into the docker context
# the script needs to already be in '.'
COPY mapd-deps-prebuilt.sh .
COPY common-functions-rockylinux.sh .

ARG PREBUILT_CUSTOM_ARG
RUN USER=root sudo bash ./mapd-deps-prebuilt.sh ${PREBUILT_CUSTOM_ARG}
