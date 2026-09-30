# Discord Channel Scroll

A [Windhawk](https://windhawk.net/) mod for Discord that lets you hold the
right mouse button and use the vertical mouse wheel to switch channels.

## Usage

- **Scroll up:** Switch to the previous channel.
- **Scroll down:** Switch to the next channel.

Regular right-clicking and horizontal scrolling are unchanged. While holding
another physical mouse button, the mod does not claim right-mouse-button input,
preserving mouse-button drag gestures.

The mod uses Discord's native `Alt+Up Arrow` and `Alt+Down Arrow` shortcuts for
channel switching, so channel ordering and behavior at the first and last
channel match Discord.

## Installation

1. Install [Windhawk](https://windhawk.net/).
2. Create a new mod in Windhawk.
3. Replace its source with `discord-channel-scroll.wh.cpp` from this repository.
4. Compile and enable the mod.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE)
or later.

## AI disclosure

This project was created with the help of LLM tools. I try to understand and review every change made by code generators, but I am an amateur and do this for fun and to create helpful tools for myself.