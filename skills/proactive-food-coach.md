# Proactive Food Coach

Give this Agent simple proactive behavior for food, fitness, and healthy-routine
requests by using the existing scheduler.

Use this Skill when the user asks for a future reminder, a later follow-up, or
a recurring routine related to meals, hydration, exercise, restaurant choices,
or low-GI eating.

## Tools

Use only existing tools:
- `get_current_time`
- `cron_add`
- `cron_list`
- `cron_remove`
- `web_search` when current restaurant information is needed now

The runtime automatically binds `cron_add` to the current conversation channel
and chat_id. Do not invent or guess a Feishu chat_id.

## Procedure

### One-time proactive reminder

When the user says things such as:
- "一小时后提醒我吃饭"
- "下午三点提醒我训练前加餐"
- "30分钟后提醒我喝水"

1. Determine the requested time.
2. Call `get_current_time` when a current timestamp is needed.
3. Compute the target UNIX timestamp.
4. Call `cron_add` with:
   - a short `name`;
   - `schedule_type="at"`;
   - integer `at_epoch`;
   - a self-contained `message`.
5. Confirm only after `cron_add` succeeds.

The reminder text should still make sense when delivered later without needing
the old conversation.

Example reminder message:
`该吃午饭啦。今天按低GI思路：先蔬菜和蛋白质，主食适量，饮料选无糖。`

### Recurring reminder

Create a recurring task only when the user explicitly requests repetition.

Use:
- `schedule_type="every"`
- `interval_s` in seconds
- a concise self-contained `message`

Do not silently create recurring reminders from a general health question.

### Cancel or inspect reminders

If the user asks what reminders exist, call `cron_list`.

If the user asks to cancel a reminder:
1. call `cron_list` if the job ID is not already known;
2. identify the intended job;
3. call `cron_remove`;
4. confirm only after successful removal.

## Combining with food advice

For a request such as:
`帮我找徐汇区适合减脂的外卖，一小时后提醒我吃饭`

Handle both parts:
1. Use the Food & Fitness Advisor behavior and `web_search` for current food
   recommendations.
2. Create the requested future reminder with `cron_add`.
3. Give one combined concise reply confirming both the recommendation and the
   scheduled follow-up.

## Proactivity rules

- Be useful, not noisy.
- Never create a reminder the user did not ask for.
- Do not create multiple overlapping reminders for one request.
- Use the current conversation as the delivery destination.
- Do not claim a reminder exists unless `cron_add` succeeded.
- Do not claim a reminder was removed unless `cron_remove` succeeded.
- For medical or urgent situations, do not substitute scheduled reminders for
  appropriate professional or emergency care.
