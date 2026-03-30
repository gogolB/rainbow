# syntax=docker/dockerfile:1.6
FROM alpine:3.20 AS builder

RUN apk add --no-cache build-base cmake git libstdc++-dev linux-headers

WORKDIR /src
COPY . .

# Build and install Abseil as static libs.
RUN git clone --depth 1 https://github.com/abseil/abseil-cpp.git /tmp/abseil \
 && cmake -S /tmp/abseil -B /tmp/abseil/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DABSL_ENABLE_INSTALL=ON \
      -DABSL_PROPAGATE_CXX_STD=ON \
      -DABSL_BUILD_TESTING=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/abseil \
 && cmake --build /tmp/abseil/build --target install -j"$(nproc)" \
 && rm -rf /tmp/abseil

# Build and install Googletest/Googlemock for containerized test builds.
RUN git clone --depth 1 https://github.com/google/googletest.git /tmp/googletest \
 && cmake -S /tmp/googletest -B /tmp/googletest/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -Dgtest_build_tests=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/googletest \
 && cmake --build /tmp/googletest/build --target install -j"$(nproc)" \
 && rm -rf /tmp/googletest

# Build rainbow as a fully static binary.
RUN cmake -S /src -B /src/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_PREFIX_PATH="/opt/abseil;/opt/googletest" \
      -DCMAKE_FIND_LIBRARY_SUFFIXES=.a \
      -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++" \
 && cmake --build /src/build -j"$(nproc)" \
 && ctest --test-dir /src/build --output-on-failure \
 && strip /src/build/rainbow

FROM scratch
COPY --from=builder /src/build/rainbow /rainbow
ENTRYPOINT ["/rainbow"]
