# ffserver

This project contains separated ffserver code which is used for RTSP server only.

To compile code run:

./buildFFServer.sh

Above command will generate a.out file.

To run RTSP server, put /etc/ffserver.conf, and it's content would be as below:

cat /etc/ffserver.conf 
RTSPPort 8554  <--------- Can be modified
<Stream test.h264> <------ sesson name
    Format rtp
    File big_buck_bunny.mp4  <------ This file will be streamed, put this file in current directory
</Stream>

Now run below command, to start RTSP server:

./a.out

Now you can start client using this url: rtsp://127.0.0.1:8554/test.h264

for example:

ffplay rtsp://127.0.0.1:8554/test.h264

which plays mp4 file.
