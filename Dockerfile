FROM ubuntu:26.04

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential ca-certificates curl libnuma-dev libpcap-dev \
        meson ninja-build pkg-config python3 python3-pyelftools xz-utils \
    && rm -rf /var/lib/apt/lists/*

ARG JOBS=4
# ponytail: cover the repository's NICs; override this list for other hardware.
ARG DPDK_DRIVERS=bus/pci,bus/vdev,mempool/ring,net/af_packet,net/e1000,net/igc,net/null,net/pcap,net/virtio,net/vmxnet3

# The IPv4 reassembly tests require the duplicate-fragment fixes in DPDK 26.07.
# Generic CPU code keeps the image portable between build and deployment hosts.
RUN curl -fL --retry 3 https://fast.dpdk.org/rel/dpdk-26.07.tar.xz -o /tmp/dpdk.tar.xz \
    && echo "7141a8b5bad9d7d965483ac0d75317ac0c21dcee1d13d373693c655f9e3fabe6  /tmp/dpdk.tar.xz" | sha256sum -c - \
    && mkdir /tmp/dpdk \
    && tar -xJf /tmp/dpdk.tar.xz -C /tmp/dpdk --strip-components=1 \
    && meson setup /tmp/dpdk/build /tmp/dpdk --prefix=/usr/local --libdir=lib \
        --buildtype=release -Dplatform=generic -Dtests=false -Ddisable_apps='*' \
        -Denable_drivers="$DPDK_DRIVERS" \
    && ninja -C /tmp/dpdk/build -j"$JOBS" \
    && ninja -C /tmp/dpdk/build install \
    && ldconfig \
    && rm -rf /tmp/dpdk /tmp/dpdk.tar.xz

WORKDIR /opt/snowtg
COPY . .

# Keep the toolchain and tests so this image can also run the regression suite.
RUN make -C pro-stack -j"$JOBS" \
    && make -C traffic-gen LDFLAGS="$(pkg-config --libs libdpdk)" \
    && make -C apps/stack-demo LDFLAGS="$(pkg-config --libs libdpdk)" \
    && make -C test -j"$JOBS" \
    && ln -s /opt/snowtg/traffic-gen/build/traffic-gen /usr/local/bin/traffic-gen \
    && ln -s /opt/snowtg/apps/stack-demo/build/stack-demo /usr/local/bin/stack-demo

# Reuse the existing end-to-end check: virtual NIC, no hugepages or PCI access.
# Override CMD with traffic-gen/stack-demo and their normal EAL arguments.
CMD ["python3", "test/test_lcore_layout.py", "/usr/local/bin/traffic-gen"]
