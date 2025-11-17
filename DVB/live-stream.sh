ffmpeg \
-readrate 1 \
-i $(yt-dlp --get-url -f 301 https://www.youtube.com/watch?v=Xk6gb4Asx_w) \
-readrate 1 \
-i $(yt-dlp --get-url -f 1080p60__source_ https://www.twitch.tv/dynamicearthcommunitylive) \
-readrate 1 \
-i https://figarotv-live.freecaster.com/live/freecaster/figarotv-audio_fre=96000-video=3500000.m3u8 \
-readrate 1 \
-i https://artesimulcast.akamaized.net/hls/live/2031003-b/artelive_fr/master_v720.m3u8 \
-map 0:v -map 0:a \
-c:v:0 copy -c:a:0 copy \
-map 1:v -map 1:a \
-c:v:1 copy -c:a:1 copy \
-map 2:v -map 2:a \
-c:v:2 copy -c:a:2 copy \
-map 3:v -map 3:a \
-c:v:3 copy -c:a:3 copy \
-program title="hd1":program_num=0x1001:st=0:st=1 \
-program title="hd2":program_num=0x1002:st=2:st=3 \
-program title="hd3":program_num=0x1003:st=4:st=5 \
-program title="hd4":program_num=0x1004:st=6:st=7 \
-pcr_period 60 \
-pat_period 0.10 \
-sdt_period 0.25 \
-pes_payload_size 0 \
-flush_packets 0 \
-mpegts_flags nit -mpegts_flags system_b \
-mpegts_service_type advanced_codec_digital_hdtv \
-max_muxing_queue_size 16 \
-muxrate 31668449.2 \
-f mpegts -y - |
tsp -v \
-I file \
-P tsrename -o 0xFF17 -t 0x0017 \
-P inject --pid 0x0010 -b 3000 nit.xml \
-P inject --pid 20 -b 1500 --stuffing time.xml \
-P timeref --system \
-P svrename -n "Youtube SKY News" -p "BillysTV" hd1 \
-P svrename -n "Twitch Earthquake" -p "BillysTV" hd2 \
-P svrename -n "Figaro" -p "BillysTV" -l "64" hd3 \
-P svrename -n "Arte" -p "BillysTV" -l "69" hd4 \
-P eitinject -f '../EIT/*.xml' --actual-schedule \
-P limit --bitrate 31668449,2 -p 0x1004 -w \
-P analyze -o mpeg-live-analyzed.txt -i 5 --error-analysis --service-analysis --ts-analysis  --wide-display \
-P analyze --json-udp 127.0.0.1:9982 -i 1 -o /dev/null --error-analysis --service-analysis --ts-analysis \
-O file mpeg-live.ts
#-readrate 1 -reconnect 1 -reconnect_at_eof 1 -reconnect_streamed 1 -reconnect_delay_max 600 -rtbufsize 100M -thread_queue_size 65536 \
#-fflags nobuffer \
#-P regulate -b 31668449.2 \

#	Sources
# plutot tv channels : 
# https://gist.github.com/miccayo/afe8aec7b01aebed7f91b9fd8a595dac
# https://www.freechannels.me/
#
# radio france :
# https://www.radiofrance.fr/comment-ecouter-radio-france#5
#
# TNT France  : 
# https://github.com/Paradise-91/ParaTV/tree/main
#
#
# To get yt-dlp ids :
# yt-dlp --list-formats URL
