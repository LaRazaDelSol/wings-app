# Host-side prototype image: the app's own build image plus OpenSSL headers.
FROM orbit-app-build:0.1
RUN apt-get update && apt-get install -y --no-install-recommends libssl-dev g++ && rm -rf /var/lib/apt/lists/*
