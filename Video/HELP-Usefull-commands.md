#Compress a file and add black bars on top and bottom so it doesn't get deformed on screen. Change the "-crf 21" to a value that correspond to the bitrate / quality needed. Lower is higher bandwith / wuality
ffmpeg -i input.ext -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1" -c:v libx264 -crf 21 -preset medium -c:a eac3 output.mkv
