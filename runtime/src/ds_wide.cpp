// Dark Sector: ultrawide (widescreen option, a screen or window wider than 16:9).
//
// The runtime shows the 16:9 frame stretched across the screen and renders
// more columns; the game's camera is given the screen's aspect so the view
// widens instead of stretching. The camera builds its projection with
// aspect = its own 16:9 (camera +1412) x sub_8223E0D8(), the display aspect
// relative to that (1.0, or the old PC version's DisplayAspectRatio setting
// when its "full-screen" flag is set). Its callers (sub_8223E1F8,
// sub_8223E398, sub_8223E4D0) are the camera's projection setup, so the
// override also marks the frame as gameplay (renderer::NoteGameplayView):
// menus and movies stay 16:9.
#include "game.h"
#if GAME_DS
#include "gpu/renderer.h"
#include "log.h"
#include "options.h"
#include "ppc_context.h"

using namespace wet;

extern "C" void __imp__sub_8223E0D8(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8223E0D8)
{
    __imp__sub_8223E0D8(ctx, base);
    if (!options::Widescreen())
        return;
    renderer::NoteGameplayView();
    float aspect = renderer::DisplayAspect();
    if (aspect <= 16.0f / 9.0f + 0.01f)
        return;
    ctx.f1.f64 = double(float(ctx.f1.f64) * (aspect / (16.0f / 9.0f)));
    static int logged = 0;
    if (logged++ < 1)
        Log("Ultrawide: camera aspect %.3f", aspect);
}
#endif
