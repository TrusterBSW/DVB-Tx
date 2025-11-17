case $1 in

  live)
	mkfifo mpeg-live.ts &> /dev/null
	screen -X -S GNU-Radio quit 2> /dev/null	#kill previous instances
	screen -S "GNU-Radio" -d -m bash -c  "python3 dvbt_tx_pipe.py" &

	screen -X -S FFMPEG quit 2> /dev/null 		#kill previous instance
	screen -S "FFMPEG" -d -m bash -c "bash live-stream.sh" &
    	;;

  file)
	mkfifo mpeg-live.ts &> /dev/null
	screen -X -S GNU-Radio quit 2> /dev/null	#kill previous instances
	screen -S "GNU-Radio" -d -m bash -c  "python3 dvbt_tx_pipe.py" &

	screen -X -S FFMPEG quit 2> /dev/null 		#kill previous instance
	screen -S "FFMPEG" -d -m bash -c "bash file-source.sh" &
    	;;

  *)
	echo "usage : start.sh source [option]"
	echo "source : file or live"
	echo "file source is using the file-source.sh script that multiplexe file file in the ../Video folder"
	echo "live source is using the live-stream.sh script that multiplexe live public stream from internet"
	echo "option : -f -g -s -h"
	echo "-f automatically attach to the screen of FFMPEG after start. Usefull to check multiplexing logs. Detach screen with ctr + a then ctrl +d"
	echo "-g automatically attech to the screen of GNU-Radio after start"
	;;
esac

case $2 in

  -f)
	sleep 2
	screen -R FFMPEG
    	;;

  -a)
	sleep 2
	watch -n 0.5 cat mpeg-live-analyzed.txt
    	;;

  -g)
	sleep 2
	screen -R GNU-Radio
	;;

  *)
	echo 'for multiplexing log do "screen -R FFMPEG"'
	echo 'for modulation log do "screen -R GNU-Radio"'
	echo 'for live analysis of the multiplex do " watch -n 1 mpeg-live-analyzed.txt"'
	;;
esac


