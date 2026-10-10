# Keep the native kbase backend separate from the system Mesa installation.
export LD_LIBRARY_PATH="/usr/lib/chopin-gpu/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBGL_DRIVERS_PATH="/usr/lib/chopin-gpu/lib/dri"
export WLR_RENDERER=gles2
export GSK_RENDERER=gl
