SHELL := /bin/zsh

ROOT_DIR := $(abspath ..)
RUN_DIR := $(CURDIR)/.run
CERT_DIR := $(RUN_DIR)/cert
HTTP_ROOT := $(RUN_DIR)/http
MEDIAMTX_CONFIG := $(RUN_DIR)/mediamtx.yml

INPUT ?= $(ROOT_DIR)/oceans.mp4
STREAM_NAME ?= imvideo
HTTP_PORT ?= 8080
HTTPS_PORT ?= 8443
RTSP_PORT ?= 8554
RTMP_PORT ?= 1935

FFMPEG ?= ffmpeg
MEDIAMTX ?= mediamtx
OPENSSL ?= openssl

HTTP_URL = http://127.0.0.1:$(HTTP_PORT)/$(notdir $(INPUT))
HTTPS_URL = https://127.0.0.1:$(HTTPS_PORT)/$(notdir $(INPUT))
RTSP_URL = rtsp://127.0.0.1:$(RTSP_PORT)/$(STREAM_NAME)
RTMP_URL = rtmp://127.0.0.1:$(RTMP_PORT)/$(STREAM_NAME)

.PHONY: help prep print-urls mediamtx http https rtsp rtmp all stop clean probe-rtsp probe-rtmp probe-http probe-https

help:
	@printf '%s\n' \
	  'streamlab targets:' \
	  '  make all         INPUT=../oceans.mp4' \
	  '  make mediamtx' \
	  '  make http' \
	  '  make https' \
	  '  make rtsp' \
	  '  make rtmp' \
	  '  make stop' \
	  '' \
	  'variables:' \
	  '  INPUT=/abs/or/relative/video.mp4' \
	  '  STREAM_NAME=imvideo' \
	  '  HTTP_PORT=8080 HTTPS_PORT=8443 RTSP_PORT=8554 RTMP_PORT=1935'

prep:
	@test -f "$(INPUT)" || (printf 'missing input: %s\n' "$(INPUT)" >&2; exit 1)
	@mkdir -p "$(RUN_DIR)" "$(HTTP_ROOT)" "$(CERT_DIR)"
	@rm -f "$(HTTP_ROOT)/$(notdir $(INPUT))"
	@ln -sf "$(abspath $(INPUT))" "$(HTTP_ROOT)/$(notdir $(INPUT))"
	@printf '%s\n' \
	  'logLevel: warn' \
	  'rtsp: yes' \
	  'rtspTransports: [tcp]' \
	  'rtspAddress: :$(RTSP_PORT)' \
	  'rtmp: yes' \
	  'rtmpAddress: :$(RTMP_PORT)' \
	  'hls: no' \
	  'webrtc: no' \
	  'paths:' \
	  '  all:' \
	  '    source: publisher' > "$(MEDIAMTX_CONFIG)"

print-urls:
	@printf '%s\n' \
	  'HTTP : $(HTTP_URL)' \
	  'HTTPS: $(HTTPS_URL) (self-signed cert)' \
	  'RTSP : $(RTSP_URL)' \
	  'RTMP : $(RTMP_URL)'

mediamtx: prep
	@if [ -f "$(RUN_DIR)/mediamtx.pid" ] && kill -0 "$$(cat "$(RUN_DIR)/mediamtx.pid")" >/dev/null 2>&1; then \
	  printf 'mediamtx already running on rtsp:%s rtmp:%s\n' "$(RTSP_PORT)" "$(RTMP_PORT)"; \
	else \
	  rm -f "$(RUN_DIR)/mediamtx.pid"; \
	  nohup "$(MEDIAMTX)" "$(MEDIAMTX_CONFIG)" > "$(RUN_DIR)/mediamtx.log" 2>&1 & echo $$! > "$(RUN_DIR)/mediamtx.pid"; \
	  sleep 1; \
	  printf 'started mediamtx on rtsp:%s rtmp:%s\n' "$(RTSP_PORT)" "$(RTMP_PORT)"; \
	fi

http: prep
	@pkill -F "$(RUN_DIR)/http.pid" >/dev/null 2>&1 || true
	@nohup python3 -m http.server "$(HTTP_PORT)" --bind 127.0.0.1 --directory "$(HTTP_ROOT)" > "$(RUN_DIR)/http.log" 2>&1 & echo $$! > "$(RUN_DIR)/http.pid"
	@printf 'started http at %s\n' "$(HTTP_URL)"

$(CERT_DIR)/localhost.crt $(CERT_DIR)/localhost.key: prep
	@"$(OPENSSL)" req -x509 -newkey rsa:2048 -nodes \
		-keyout "$(CERT_DIR)/localhost.key" \
		-out "$(CERT_DIR)/localhost.crt" \
		-days 1 -subj '/CN=127.0.0.1' >/dev/null 2>&1

https: prep $(CERT_DIR)/localhost.crt $(CERT_DIR)/localhost.key
	@pkill -F "$(RUN_DIR)/https.pid" >/dev/null 2>&1 || true
	@nohup python3 -c "import http.server, ssl, socketserver; h=http.server.SimpleHTTPRequestHandler; s=socketserver.TCPServer(('127.0.0.1', $(HTTPS_PORT)), lambda *a, **k: h(*a, directory='$(HTTP_ROOT)', **k)); c=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain('$(CERT_DIR)/localhost.crt','$(CERT_DIR)/localhost.key'); s.socket=c.wrap_socket(s.socket, server_side=True); s.serve_forever()" > "$(RUN_DIR)/https.log" 2>&1 & echo $$! > "$(RUN_DIR)/https.pid"
	@printf 'started https at %s\n' "$(HTTPS_URL)"

rtsp: mediamtx prep
	@pkill -F "$(RUN_DIR)/rtsp.pid" >/dev/null 2>&1 || true
	@nohup "$(FFMPEG)" -re -stream_loop -1 -i "$(INPUT)" -map 0:v:0 -map '0:a:0?' -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params 'repeat-headers=1' -c:a aac -ar 48000 -ac 2 -f rtsp -rtsp_transport tcp "$(RTSP_URL)" > "$(RUN_DIR)/rtsp.log" 2>&1 & echo $$! > "$(RUN_DIR)/rtsp.pid"
	@sleep 1
	@printf 'started rtsp publisher at %s\n' "$(RTSP_URL)"

rtmp: mediamtx prep
	@pkill -F "$(RUN_DIR)/rtmp.pid" >/dev/null 2>&1 || true
	@nohup "$(FFMPEG)" -re -stream_loop -1 -i "$(INPUT)" -map 0:v:0 -map '0:a:0?' -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -bf 0 -g 30 -keyint_min 30 -sc_threshold 0 -x264-params 'repeat-headers=1' -c:a aac -ar 48000 -ac 2 -f flv "$(RTMP_URL)" > "$(RUN_DIR)/rtmp.log" 2>&1 & echo $$! > "$(RUN_DIR)/rtmp.pid"
	@sleep 2
	@printf 'started rtmp publisher at %s\n' "$(RTMP_URL)"

all: http https rtsp rtmp print-urls

probe-http:
	@ffprobe -hide_banner -loglevel error "$(HTTP_URL)"

probe-https:
	@ffprobe -hide_banner -loglevel error "$(HTTPS_URL)"

probe-rtsp:
	@ffprobe -hide_banner -loglevel error "$(RTSP_URL)"

probe-rtmp:
	@ffprobe -hide_banner -loglevel error "$(RTMP_URL)"

stop:
	@for name in rtmp rtsp https http mediamtx; do \
	  pkill -F "$(RUN_DIR)/$$name.pid" >/dev/null 2>&1 || true; \
	  rm -f "$(RUN_DIR)/$$name.pid"; \
	done
	@printf 'stopped streamlab processes\n'

clean: stop
	@rm -rf "$(RUN_DIR)"
	@printf 'cleaned streamlab runtime files\n'
