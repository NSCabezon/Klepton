# Third-party notices

## Glovebox

Parts of this fork are adapted from Glovebox (github.com/glovebox-vr/glovebox),
used under the MIT License below. Each adapted file or function says so in a
comment naming its Glovebox source:

- `runtime/gfx/kl_gpu_time.{m,h}` and `klvk_gpu_watch` in `runtime/gfx/kl_vulkan.c`
  (KL_GPU_TIME)
- the guest frame pacer in `visionos/Sources/kl_app.c` (KL_GUEST_PACE_HZ / _DIV)
- the `tkill(tid, 0)` probe in `runtime/libc/kl_libc.c` (KL_TKILL_PROBE)
- the Alyx render convar list in `runtime/guest/kl_glibc.c` (KL_ALYX_PROFILE)

```
MIT License

Copyright (c) 2026 the Glovebox contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
