# The MediaTek V4L2 decoders (driven by the stock vpud) produce bit-exact
# frames for CPU consumers, but GStreamer's GL upload of their tiled MM21
# output shows corrupted pixels in players and browsers (glimagesink, WebKit).
# Keep them out of automatic decoder selection so applications decode in
# software; explicit pipelines can still use v4l2h264dec and friends.
case ",$GST_PLUGIN_FEATURE_RANK," in
*,v4l2h264dec:NONE,*) ;;
*)
	export GST_PLUGIN_FEATURE_RANK="v4l2h263dec:NONE,v4l2h264dec:NONE,v4l2mpeg2dec:NONE,v4l2mpeg4dec:NONE,v4l2vp8dec:NONE,v4l2vp9dec:NONE${GST_PLUGIN_FEATURE_RANK:+,$GST_PLUGIN_FEATURE_RANK}"
	;;
esac
