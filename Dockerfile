FROM gcc:14
RUN apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY resources ./resources
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 2 && ctest --test-dir build --output-on-failure
ENTRYPOINT ["./build/elevator_simulator"]
CMD ["--time-scale", "0.01"]
