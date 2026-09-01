# Vibepollo, with guest invites

> This is a fork of [Nonary/Vibepollo](https://github.com/Nonary/Vibepollo), which is
> itself a fork of Apollo/Sunshine. Everything below the line is Nonary's; this
> section covers only what this fork adds. Upstream's README follows unchanged.

Hand someone a link and they can play. No account, no password, and nothing to
install if they would rather use a browser. The owner creates an invite; the guest
opens the link, types a PIN sent separately, and either plays in the browser or
pairs a Moonlight client they already have. A guest never reaches the admin
interface.

## What an invite decides

- **What they can do** — watch only, gamepad only, or full keyboard and mouse.
- **How long, and how many times** — invites expire, can be capped to a number of
  uses, and survive a restart.
- **Where the link points** — set `public_base_url` and links are built from the
  address guests actually reach the host on.

Permissions are applied on the host, never asked of the client:

- A browser session has its permissions overwritten every time one is created.
- A paired Moonlight client is re-checked against the invite at launch, so
  narrowing an invite narrows a device that was paired under the old terms.
- Revoking ends what is already running — the browser session is closed and a
  Moonlight client paired through the invite is unpaired — rather than merely
  refusing the next connection.

## Playing on a phone

- On-screen controller with twin sticks, d-pad, face buttons, bumpers and triggers,
  for guests and for the owner's own browser stream.
- Not a second input path: it publishes a gamepad the existing capture reads as
  hardware, so it inherits the same permissions and the host cannot tell it apart.
- Slide a finger from one button to another to hold both — combos do not need a
  third thumb.
- Triggers are analog: a tap is a full pull, sliding eases off.
- Bumpers and triggers sit along the top edge, where the index fingers already rest.
- Every cluster can be dragged and resized, saved per browser.
- Tilt can drive the right stick while no thumb is on it, so aiming and pressing
  face buttons stop competing for the same thumb.
- Rumble works on Android through the vibration motor, approximated with a duty
  cycle because the API has no notion of intensity. iOS does not expose vibration.
- Add it to a homescreen and it opens without browser chrome.

## Streaming in a browser

- Quality is chosen before connecting rather than after, because the encoder is
  fixed when a session starts, and the choice is remembered.
- Offers whichever of H.264, HEVC and AV1 the browser reports it can decode.
- **Every viewer gets their own encoder.** Peers no longer share one, so they no
  longer share a codec, a bitrate or a keyframe stream — one guest on a poor line
  cannot drag everyone else down, and a guest asking for keyframes no longer
  injects them into the owner's stream.
- Optional client-side FSR sharpening for streams displayed larger than they are
  encoded.
- Gamepad slots are handed out on join and returned on leave, so two people do not
  fight over controller one.

## Knowing whether it works

- **Connection test** on both the admin page and the guest pre-flight: reports
  whether the route would be direct or relayed, and names the reason when it is
  neither. It talks only to the ICE servers, so it starts no stream and uses no
  invite.
- **Link quality** as signal bars during a stream, from round-trip time, jitter and
  packet loss — reporting the worst of the three and which one it is, since the
  fixes are unrelated.

## Relay and access

- `webrtc_ice_servers` configures STUN and TURN for guests whose network will not
  allow a direct connection.
- `webrtc_turn_secret` mints short-lived credentials per session (coturn's REST
  scheme), so a guest let in once cannot keep using the relay, and cutting one off
  does not mean changing a password every other guest holds.

## Maturity

The guest path has had far more attention than mileage. It has been exercised end to
end — a guest joining from another continent, on a phone, over a VPN — but by two
people over a few days, not by a userbase. Treat the permission model as the
load-bearing part and the rest as young.

---

# Vibepollo

## What is Vibepollo?

Vibepollo is an AI‑enhanced version of Apollo, a popular remote streaming application. It intends to integrate all scripts from myself (Nonary) and more.



## Key Features

* **Display Setting Automation**
  Vibepollo adds multiple safeguards to prevent dummy plugs or virtual displays from getting “stuck” when you return to your PC. It resolves common Windows 11 **24H2** display issues and restores your layout after hard crashes, shutdowns, or reboots. (The only scenario it can’t restore is during a user logout.) The workflow is simplified to a dropdown—just pick the display you want to stream.

* **Windows Graphics Capture in Service Mode**
  Running Windows Graphics Capture (WGC) as a service improves performance and stability. It captures the full frame rate of frame‑generated titles, avoids crashes when VRAM is exceeded, and follows Microsoft’s recommended capture method going forward. Vibepollo auto‑switches capture methods on demand, so the login screen and UAC prompts are still captured even when using WGC.

* **Native Virtualized Display**
  Vibepollo uses its bundled virtual display driver by default and keeps SudoVDA installed as a rollback option. It can capture output from any GPU, including those in hybrid laptops, ensuring the virtual screen connects to the correct GPU when needed. It also provides simple virtual display options, allowing users to choose between a physical or virtual display. On headless setups, it enables automatically to prevent 503 errors and false encoder detections, such as incorrect HEVC support reports.

* **Focused Configuration Interface**
  Vibepollo includes a responsive, dependency-light browser interface built around the tasks people perform most often: selecting a streaming display, tuning frame pacing, managing games and devices, checking sessions, and recovering the host. Less common controls remain organized by domain instead of competing with everyday setup.

* **Playnite Integration**
  Deep integration with Playnite (a “launcher of launchers”) automatically syncs your recently played games with configurable expiration rules, per‑category sync, and exclusions. You can also add games manually from a Web UI dropdown; Vibepollo handles artwork, launching, and clean termination—emulators included. The goal is a seamless, GeForce Experience–style library experience—only better.

* **RTSS & NVIDIA Control Panel Integration**
  Vibepollo can manage RTSS to apply the correct frame limit and disable V‑Sync before streaming, significantly improving frame pacing and smoothness. The applied frame cap matches the client device’s requested FPS.

* **Frame‑Generated Capture Fixes**
  DLSS/FSR game-provided frame generation requires Vibepollo's virtual screen for reliable capture. The virtual display guarantees composed flip, allowing generated frames to be captured through WGC, and Vibepollo targets 4x virtual refresh for pacing.

* **Lossless Scaling & NVIDIA Smooth Motion**
  Vibepollo can automatically apply optimal Lossless Scaling settings to generate frames for any application. On RTX 40‑series and newer GPUs, you can optionally enable **NVIDIA Smooth Motion** for better performance and image quality (while Lossless Scaling remains more customizable).

* **API Token Management**
  Access tokens can be tightly scoped—down to specific methods—so external scripts don’t need full administrative rights. This improves security while keeping automation flexible.

* **Session‑Based Authentication**
  The sign‑in flow supports password managers and includes a “remember me” option to minimize prompts. The experience is security‑hardened without sacrificing convenience.

* **Update Notifications**
  Built‑in notifications let you know when new features or bug fixes are available, making it easy to stay current.

Due to the sheer pace and volume of changes I was producing, it became impractical to manage them within the original Sunshine repository. The review process simply couldn’t keep up with the rate of development, and large feature sets were piling up without a clear path to integration. To ensure the work remained organized, maintainable, and actively progressing, I established Vibepollo as a standalone fork.

At this point, Vibepollo differs substantially from upstream Sunshine. At that scale, asking upstream maintainers to accept large backports in one sweep is generally not sustainable, which is why Vibepollo continues as a standalone fork.

---

## Does Vibepollo aim to replace Sunshine or Apollo?

No. Vibepollo is intended as a **complementary fork**, not a replacement.


## Will Vibepollo’s features merge back into Sunshine or Apollo?

**Short answer: Unlikely to be backported upstream as large, sweeping merges.**

Vibepollo is largely AI‑generated. While it works well, it carries a kind of surface‑level technical debt that many upstream projects want resolved before taking big changes (styling consistency, thin/missing docs, and some over‑engineering). I see that debt as relatively unimportant today because modern AI tools can answer “why does this function exist?”, “what does this parameter do?”, or “how do these classes interact?” and will soon auto‑fix these issues—re‑style trees, write docstrings, and prune unused layers—without human effort.

So this “mess” is mostly cosmetic. It doesn’t break the code, create security risks, or block future maintenance. The only debt that truly matters is architectural: API design, threading models, modularity, and performance. Those are hard to fix even with AI tools, which is why I focus on them up front and guide the AI accordingly.

Because I define the architecture, I know how everything works. Whether the code looks polished or not doesn’t matter to me.

Bringing Vibepollo fully in line with upstream style and documentation would take a lot of engineering time for limited practical gain. For now, full backports into Sunshine or Apollo are unlikely. Over time, targeted refactors or added documentation may make **selective upstreaming** possible.

---

## Origin of the Name "Vibepollo"

The name arose as a playful suggestion from another developer who joked about the potential unmanageability of extensive AI‑generated code. Given that approximately **99% of Vibepollo’s code is AI‑generated**, the name seemed fitting.

---

## Why Use AI‑generated Code? Concerns About Technical Debt?

AI significantly accelerates development by offloading much of the routine implementation work. Instead of spending hours writing boilerplate, wiring dependencies, or handling repetitive edge cases, I can focus on high‑level architecture, long‑term design decisions, and system direction. This shift doesn’t just speed things up—it fundamentally changes the role of the engineer, pushing us toward oversight, orchestration, and design rather than rote code production.

What stands out most is that AI code works on the first try around 90% of the time. That reliability, combined with instant generation, makes it dramatically more efficient to accept its form of debt than to painstakingly write everything from scratch. In other words, I’m trading minor, manageable debt for massive development velocity—and that trade is almost always worth it.

I’m not overly concerned about technical debt in this workflow, because the debt that truly matters stems from bad architecture and poor design choices, not from the code itself. As long as I guide the AI with clear structure and intent, the generated code ends up being maintainable. Problems like inconsistent naming, redundant code, or unused helpers are minor forms of debt—easily identified, cleaned up, or ignored. By contrast, deep architectural flaws, poor layering, or mismatched abstractions create lasting problems.

In fact, compared to many traditional enterprise codebases I’ve maintained, AI‑assisted code often comes out cleaner and easier to manage. Legacy systems are usually burdened with years of ad‑hoc patches, inconsistent styles, and various bad practices due to knowledge level of contributor. AI‑generated code doesn’t necessarily carry fewer design flaws than human code, but it does avoid accumulating those scars—especially when paired with an intentional architectural vision, and it is less likely to do seriously bad practices that you typically find in enterprise codebases.

Broadly speaking, AI‑assisted development represents the future of software engineering. Just as compilers and IDEs once transformed programming, AI is now transforming how we design, implement, and maintain systems. Instead of fearing it, I view it as a force multiplier that complements professional judgment. Vibepollo is an example of what happens when you embrace that shift: rapid iteration, a massive expansion of features, and code that remains maintainable because the architecture is intentionally guided.

---

## The Original “AI-Only” Goal (And Why It Changed)

One of the original goals of Vibepollo was to prove a specific point: that an experienced developer could maintain a complex project using almost entirely AI‑generated code, as long as they provided the architecture and kept the system coherent.

That idea hasn’t aged particularly well, not because it was wrong, but because the models scaled far faster than most projections. The result is that the “skill gap” in prompting and guiding the AI matters less than it did even a few months prior. You still need engineering judgment and architecture, but it’s now dramatically easier to get high‑quality, end‑to‑end results without the same level of careful orchestration. So the original “prove it’s possible” goal is basically moot: it’s not a niche workflow anymore, it’s simply where the tools have gone.

---

## AI Models Used by Vibepollo

Vibepollo has always been built with **Codex** as the primary workflow, and in practice that has meant mostly the **GPT‑5 family** (today: **GPT‑5.3‑Codex**). I use it with the same principles as before: start from architecture, sanity‑check assumptions, and do the hard reasoning up front so the implementation lands cleanly.

With **GPT‑5.3‑Codex**, there’s no real need to juggle a “fast but less capable” model anymore. In the past I’d reach for speed‑first models (like Sonnet, or smaller GPT “mini” variants) for quick turnaround, but **GPT‑5.3‑Codex** covers both: it’s about as fast as those options while also being strong enough to handle the hard engineering work in one pass.

Claude was used more heavily earlier on. Older Claude models had a tendency to go off on their own path, even when the architectural plan was clear. That behavior has mostly been fixed in newer Claude releases, but GPT still ended up being the more useful engineering tool for me because it will challenge you and not simply agree with whatever you ask for.

In general, GPT has felt more intelligent for the way I build and maintain this codebase. I may occasionally ask **Claude Opus 4.5** for a second opinion if GPT can’t resolve something cleanly end‑to‑end, but this is increasingly rare.

---

## Sponsors

<p align="center">
  <a href="https://signpath.io?utm_source=foundation&amp;utm_medium=github&amp;utm_campaign=vibepollo">
    <img src="docs/images/signpath.svg" alt="SignPath" width="420">
  </a>
</p>

Thank you to [SignPath.io](https://signpath.io?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo)
and the [SignPath Foundation](https://signpath.org?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo)
for sponsoring Vibepollo's Windows code signing.

### Code signing policy

Official Vibepollo Windows releases use free code signing provided by
[SignPath.io](https://signpath.io?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo), and a
certificate by the [SignPath Foundation](https://signpath.org?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo).

* **Committer and reviewer:** [Nonary](https://github.com/Nonary)
* **Approver:** [Nonary](https://github.com/Nonary)
* **Privacy:** Vibepollo transfers information to networked systems only for functionality requested by the user or
  operator; it does not transmit user or runtime data to SignPath. Separately, SignPath's GitHub integration receives
  the build artifacts, signing-request details, and GitHub-provided build-origin metadata needed to sign official
  releases.
