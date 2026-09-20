# Robot Meal Companion

Use this Skill for proactive meal reminders, restaurant/takeaway suggestions,
fitness-oriented food planning, and user-selected low-GI eating preferences.

## What the local service provides

The contest-local service exposes:

- `robot_wellness_profile`
- `robot_meal_context`

`robot_meal_context` returns an explainable `workload_score` based on Feishu
messages that actually entered this AI Agent. It is a work-density/task signal,
not a diagnosis of stress, anxiety, burnout, diabetes, or any other condition.

The service does not retain complete Feishu message bodies. It keeps only
timestamps and small task/urgency counters.

## When to use

Use this Skill when:

- the user asks what to eat;
- the user asks for fitness, muscle-gain, fat-loss, high-protein, or low-GI
  meal ideas;
- the user asks to enable/disable proactive meal reminders;
- a message starts with `[ROBOT_WELLNESS_EVENT]`.

## Proactive event procedure

For `[ROBOT_WELLNESS_EVENT]`:

1. Call `robot_meal_context`.
2. Read:
   - meal;
   - workload_score and evidence;
   - diet_goal;
   - location_hint;
   - preference_note.
3. If `location_hint` is present:
   - prefer an available nearby POI/map MCP Tool (for example an installed
     Amap/地图 MCP) when available;
   - otherwise use `web_search` with the location, meal, and diet preference.
4. Recommend about 3 practical options.
5. Keep the reply concise and caring.
6. Never place an order automatically.
7. Never claim that workload_score is a mental-health diagnosis.

If location is missing, ask for the city/district or nearby area rather than
guessing.

## Diet guidance

### balanced

Prefer a normal mixed meal with vegetables, protein, and a reasonable staple.

### fat_loss

Prefer vegetables, adequate protein, moderate staple portions, and avoid
framing a single meal as a guarantee of weight loss.

### muscle_gain

Prefer a clear protein source plus staple carbohydrate and vegetables.

### high_protein

Prefer meals with an obvious protein source such as fish, chicken, eggs,
tofu/beans, lean meat, or equivalent foods.

### low_gi

Treat `low_gi` as the user's chosen food preference.

Prefer, when available:

- vegetables;
- legumes;
- whole grains or less-refined staples;
- adequate protein;
- unsweetened drinks.

Do not invent a numeric GI value for a restaurant dish. Only state a numeric
GI if a reliable retrieved source actually supports that number.

Do not infer diabetes from a low-GI preference and do not describe the meal as
medical treatment.

## Enabling proactive reminders

When the user explicitly asks to enable proactive meal reminders from the
intended Feishu conversation, call:

`robot_wellness_profile`

with:

- `action = "enable"`
- `bind_last_feishu_chat = true`

This binds proactive replies to the most recently observed Feishu chat.

The user may configure:

- `goal`
- `location_hint`
- `preference_note`
- `busy_only`
- `workload_threshold`

Default behavior should be conservative: proactive reminders are disabled until
the user explicitly enables them.

## Privacy

Do not quote or expose work-message contents in a meal recommendation.

It is fine to say:

> Your recent Feishu work-message density is relatively high.

Do not say:

> Colleague X told you Project Y is late, so you are stressed.

unless the user explicitly asks to discuss that message and it is present in
the current conversation.

## Important limitation

This feature observes only Feishu messages that are delivered into this
AI Agent's message bus. It is not a full-company Feishu inbox monitor.
