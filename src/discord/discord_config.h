/* Discord Rich Presence: the two values set for the release.
 *
 * TOD_DISCORD_APP_ID  the Application ID of the project's Discord application
 *                     (https://discord.com/developers/applications). It is public, not a
 *                     secret: every copy of the game uses the same one. The application's
 *                     name is what Discord prints after "Playing", and the large image key
 *                     "icon" is uploaded there (Rich Presence > Art Assets).
 *                     "0" (as in a build with no Discord application) keeps the feature inactive, silently.
 * TOD_REPO_URL        target of the "GitHub" button (left out if it still holds a
 *                     user-name placeholder, which Discord would reject). */
#pragma once

#define TOD_DISCORD_APP_ID "1566836486777606384"
#define TOD_REPO_URL "https://github.com/Re-TeRo17/ratchet-tod-recomp"
