# Sourced by visionos/mkguest.sh with ROOT, SRC and KLT_NAME set.
# Walkabout's ownership interface asks BIsDlcInstalled, but its Android course
# bundles live outside Steam depot storage. Redirect only that consumer to the
# genuine subscription query, on a hash-pinned copy before signing. Keep the
# standard patch A/B controls and never modify the supplied game library.
GUEST_PATCH_INPUT=""
case ",${KL_GUEST_PATCH_OFF:-}," in
  *,walkabout-dlc-ownership,*) DLC_OFF=1 ;;
  *) DLC_OFF=0 ;;
esac
if [ "$KLT_NAME" = walkabout-57013 ] && [ "${KL_GUEST_PATCH:-1}" != 0 ] && [ "$DLC_OFF" = 0 ]; then
  GUEST_PATCH_INPUT="$ROOT/build/walkabout-dlc-compat/libil2cpp.so"
  python3 "$ROOT/games/walkabout/tools/walkabout_dlc_compat.py" --binary "$SRC/libil2cpp.so" --out "$GUEST_PATCH_INPUT"
fi
