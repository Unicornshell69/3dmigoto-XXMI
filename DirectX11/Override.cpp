#include "Override.h"

#include "Globals.h"
#include "D3D11Wrapper.h"
#include "IniHandler.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <strsafe.h>
#include <algorithm>

PresetOverrideMap presetOverrides;

OverrideTransition CurrentTransition;
OverrideGlobalSave OverrideSave;

Override::Override()
{
	is_conditional = false;

	active = false;
}

void Override::ParseIniSection(LPCWSTR section)
{
	IniSectionVector *section_vec = NULL;
	IniSectionVector::iterator entry;
	CommandListVariable *var = NULL;
	float DirectX::XMFLOAT4::*param_component;
	int param_idx;
	float val;
	wchar_t buf[MAX_PATH];
	wstring ini_namespace;

	get_section_namespace(section, &ini_namespace);

	GetIniSection(&section_vec, section);
	for (entry = section_vec->begin(); entry < section_vec->end(); entry++) {
		if (ParseIniParamName(entry->first.c_str(), &param_idx, &param_component)) {
			val = GetIniFloat(section, entry->first.c_str(), FLT_MAX, NULL);
			if (val != FLT_MAX) {
				// Reserve space in IniParams for this variable:
				G->iniParamsReserved = max(G->iniParamsReserved, param_idx + 1);

				mOverrideParams[OverrideParam(param_idx, param_component)] = val;
			}
		} else if (entry->first.c_str()[0] == L'$') {
			if (!parse_command_list_var_name(entry->first.c_str(), &entry->ini_namespace, &var)) {
				LogOverlayW(LOG_WARNING, L"Undeclared variable %ls\n - [%ls] @ [%ls]\n", entry->first.c_str(), section, entry->ini_namespace.c_str());
				continue;
			}

			val = GetIniFloat(section, entry->first.c_str(), FLT_MAX, NULL);
			if (val != FLT_MAX) {
				mOverrideVars[var] = val;
			}
		}
	}

	transition = GetIniInt(section, L"transition", 0, NULL);
	release_transition = GetIniInt(section, L"release_transition", 0, NULL);

	transition_type = GetIniEnumClass(section, L"transition_type", TransitionType::LINEAR, NULL, TransitionTypeNames);
	release_transition_type = GetIniEnumClass(section, L"release_transition_type", TransitionType::LINEAR, NULL, TransitionTypeNames);

	if (GetIniStringAndLog(section, L"condition", 0, buf, MAX_PATH)) {
		wstring sbuf(buf);
		// Expressions are case insensitive:
		std::transform(sbuf.begin(), sbuf.end(), sbuf.begin(), ::towlower);

		if (!condition.parse(&sbuf, &ini_namespace, NULL)) {
			LogOverlayW(LOG_WARNING, L"Invalid condition = \"%ls\"\n - [%ls] @ [%ls]\n", buf, section, ini_namespace.c_str());
		} else {
			is_conditional = true;
		}
	}

	// The keys/presets sections have too much conflicting syntax to be
	// turned into a command list (at least not easily - transitions &
	// cycles are the ones that complicate matters), but we still want to
	// be able to trigger command lists on keys, so we allow for a run
	// command to trigger a command list. Since this run command is a
	// subset of the command list syntax it does not itself make it any
	// harder to turn the entire section into a command list if we wanted
	// to in the future, provided we can deal with the other problems.
	if (GetIniStringAndLog(section, L"run", NULL, buf, MAX_PATH)) {
		wstring sbuf(buf);

		if (!ParseRunExplicitCommandList(section, L"run", &sbuf, NULL, &activate_command_list, &deactivate_command_list, &ini_namespace))
			LogOverlayW(LOG_WARNING, L"Invalid run=\"%ls\"\n - [%ls] @ [%ls]\n", sbuf.c_str(), section, ini_namespace.c_str());
	}
}

struct KeyOverrideCycleParam
{
	std::string cur;
	std::string buf;
	const char *ptr;

	KeyOverrideCycleParam() :
		ptr(NULL)
	{}

	bool next()
	{
		const char *t1, *t2;

		if (!ptr)
			ptr = buf.c_str();

		// Done?
		if (!*ptr)
			return false;

		// Skip over whitespace:
		for (; *ptr == ' '; ptr++) {}

		// Mark start of current entry:
		t1 = ptr;

		// Scan until the next comma or end of string:
		for (; *ptr && *ptr != ','; ptr++) {}

		// Scan backwards over any trailing whitespace in this entry:
		for (t2 = ptr - 1; t2 >= t1 && *t2 == ' '; t2--) {}

		// If it's a comma advance to the next item:
		if (*ptr == ',')
			ptr++;

		// Extract this entry:
		cur = std::string{t1, (uintptr_t)(++t2 - t1)};

		return true;
	}

	void log(const wchar_t *name)
	{
		if (!cur.empty())
			LogInfoNoNL(" %S=%s", name, cur.c_str());
	}

	float as_float(float default)
	{
		float val;
		int n;

		n = sscanf_s(cur.c_str(), "%f", &val);
		if (!n || n == EOF) {
			// Blank entry
			return default;
		}
		return val;
	}

	int as_int(int default)
	{
		int val;
		int n;

		n = sscanf_s(cur.c_str(), "%i", &val);
		if (!n || n == EOF) {
			// Blank entry
			return default;
		}
		return val;
	}

	template <class T1, class T2>
	T2 as_enum(EnumName_t<T1, T2> *enum_names, T2 default)
	{
		T2 val;

		if (cur.empty()) {
			// Blank entry
			return default;
		}

		val = lookup_enum_val<T1, T2>(enum_names, cur.c_str(), (T2)-1);
		if (val == (T2)-1) {
			LogOverlayW(LOG_WARNING, L"Unmatched value \"%S\"\n", cur.c_str());
			return default;
		}

		return val;
	}

	bool as_expression(LPCWSTR section, CommandListExpression *expression)
	{
		wstring scur(cur.begin(), cur.end());
		wstring ini_namespace;

		if (cur.empty()) {
			// Blank entry
			return false;
		}

		get_section_namespace(section, &ini_namespace);

		// Expressions are case insensitive:
		std::transform(scur.begin(), scur.end(), scur.begin(), ::towlower);

		if (!expression->parse(&scur, &ini_namespace, NULL)) {
			LogOverlayW(LOG_WARNING, L"Invalid condition=\"%S\"\n - [%ls] @ [%ls]\n", cur.c_str(), section, ini_namespace.c_str());
			return false;
		}

		return true;
	}

	void as_run_command(LPCWSTR section, CommandList *pre_command_list, CommandList *deactivate_command_list)
	{
		wstring scur(cur.begin(), cur.end());
		wstring ini_namespace;

		if (cur.empty()) {
			// Blank entry
			return;
		}

		get_section_namespace(section, &ini_namespace);

		if (!ParseRunExplicitCommandList(section, L"run", &scur, NULL, pre_command_list, deactivate_command_list, &ini_namespace))
			LogOverlayW(LOG_WARNING, L"Invalid run=\"%S\"\n - [%ls] @ [%ls]\n", cur.c_str(), section, ini_namespace.c_str());
	}
};

void KeyOverrideCycle::ParseIniSection(LPCWSTR section)
{
	std::map<OverrideParam, struct KeyOverrideCycleParam> param_bufs;
	std::map<OverrideParam, struct KeyOverrideCycleParam>::iterator j;
	std::map<CommandListVariable*, struct KeyOverrideCycleParam> var_bufs;
	std::map<CommandListVariable*, struct KeyOverrideCycleParam>::iterator k;
	struct KeyOverrideCycleParam transition, release_transition;
	struct KeyOverrideCycleParam transition_type, release_transition_type;
	struct KeyOverrideCycleParam condition;
	struct KeyOverrideCycleParam run;
	bool not_done = true;
	int i;
	wchar_t buf[8];
	OverrideParams params;
	OverrideVars vars;
	bool is_conditional;
	CommandListExpression condition_expression;
	CommandList activate_command_list, deactivate_command_list;
	IniSectionVector *section_vec = NULL;
	IniSectionVector::iterator entry;
	CommandListVariable *var = NULL;
	float DirectX::XMFLOAT4::*param_component;
	int param_idx;
	float val;

	wrap = GetIniBool(section, L"wrap", true, NULL);
	smart = GetIniBool(section, L"smart", true, NULL);

	GetIniSection(&section_vec, section);
	for (entry = section_vec->begin(); entry < section_vec->end(); entry++) {
		if (ParseIniParamName(entry->first.c_str(), &param_idx, &param_component)) {
			// Reserve space in IniParams for this variable:
			G->iniParamsReserved = max(G->iniParamsReserved, param_idx + 1);

			GetIniString(section, entry->first.c_str(), 0, &param_bufs[OverrideParam(param_idx, param_component)].buf);
		} else if (entry->first.c_str()[0] == L'$') {
			if (!parse_command_list_var_name(entry->first.c_str(), &entry->ini_namespace, &var)) {
				LogOverlayW(LOG_WARNING, L"Undeclared variable %ls\n - [%ls] @ [%ls]\n", entry->first.c_str(), section, entry->ini_namespace.c_str());
				continue;
			}

			GetIniString(section, entry->first.c_str(), 0, &var_bufs[var].buf);
		}
	}

	GetIniString(section, L"transition", 0, &transition.buf);
	GetIniString(section, L"release_transition", 0, &release_transition.buf);
	GetIniString(section, L"transition_type", 0, &transition_type.buf);
	GetIniString(section, L"release_transition_type", 0, &release_transition_type.buf);
	GetIniString(section, L"condition", 0, &condition.buf);
	GetIniString(section, L"run", 0, &run.buf);

	for (i = 1; not_done; i++) {
		not_done = false;

		for (j = param_bufs.begin(); j != param_bufs.end(); j++)
			not_done = j->second.next() || not_done;

		for (k = var_bufs.begin(); k != var_bufs.end(); k++)
			not_done = k->second.next() || not_done;

		not_done = transition.next() || not_done;
		not_done = release_transition.next() || not_done;
		not_done = transition_type.next() || not_done;
		not_done = release_transition_type.next() || not_done;
		not_done = condition.next() || not_done;
		not_done = run.next() || not_done;

		if (!not_done)
			break;

		LogInfoNoNL("  Cycle %i:", i);
		params.clear();
		for (j = param_bufs.begin(); j != param_bufs.end(); j++) {
			val = j->second.as_float(FLT_MAX);
			if (val != FLT_MAX) {
				StringCchPrintf(buf, 8, L"%c%.0i", j->first.chr(), j->first.idx);
				j->second.log(buf);
				params[j->first] = val;
			}
		}
		vars.clear();
		for (k = var_bufs.begin(); k != var_bufs.end(); k++) {
			val = k->second.as_float(FLT_MAX);
			if (val != FLT_MAX) {
				k->second.log(k->first->name.c_str());
				vars[k->first] = val;
			}
		}

		is_conditional = condition.as_expression(section, &condition_expression);
		run.as_run_command(section, &activate_command_list, &deactivate_command_list);

		transition.log(L"transition");
		release_transition.log(L"release_transition");
		transition_type.log(L"transition_type");
		release_transition_type.log(L"release_transition_type");
		condition.log(L"condition");
		run.log(L"run");
		LogInfo("\n");

		presets.push_back(KeyOverride(KeyOverrideType::CYCLE, &params, &vars,
			transition.as_int(0), release_transition.as_int(0),
			transition_type.as_enum<const char *, TransitionType>(TransitionTypeNames, TransitionType::LINEAR),
			release_transition_type.as_enum<const char *, TransitionType>(TransitionTypeNames, TransitionType::LINEAR),
			is_conditional, condition_expression, activate_command_list, deactivate_command_list));
	}
}

bool Override::MatchesCurrent(HackerDevice *device)
{
	OverrideParams::iterator i;
	OverrideVars::iterator j;
	float val;

	for (i = begin(mOverrideParams); i != end(mOverrideParams); i++) {
		std::map<OverrideParam, OverrideTransitionParam>::iterator transition = CurrentTransition.params.find(i->first);
		if (transition != CurrentTransition.params.end() && transition->second.time != -1)
			val = transition->second.target;
		else
			val = G->iniParams[i->first.idx].*i->first.component;

		if (i->second != val)
			return false;
	}

	for (j = begin(mOverrideVars); j != end(mOverrideVars); j++) {
		std::map<CommandListVariable*, OverrideTransitionParam>::iterator transition = CurrentTransition.vars.find(j->first);
		if (transition != CurrentTransition.vars.end() && transition->second.time != -1)
			val = transition->second.target;
		else
			val = j->first->fval;

		if (j->second != val)
			return false;
	}

	return true;
}

void KeyOverrideCycle::UpdateCurrent(HackerDevice *device)
{
	// If everything in the current preset matches reality or the current
	// transition target we are good:
	if (current >= 0 && (size_t)current < presets.size() && presets[current].MatchesCurrent(device))
		return;

	// The current preset doesn't match reality - we've got out of sync.
	// Search for any other presets that do match:
	for (unsigned i = 0; i < presets.size(); i++) {
		if (i != current && presets[i].MatchesCurrent(device)) {
			LogInfo("Resynced key cycle: %i -> %i\n", current, i);
			current = i;
			return;
		}
	}
}

void KeyOverrideCycle::DownEvent(HackerDevice *device)
{
	if (presets.empty())
		return;

	if (smart)
		UpdateCurrent(device);

	if (current == -1)
		current = 0;
	else if (wrap)
		current = (current + 1) % presets.size();
	else if ((unsigned)current < presets.size() - 1)
		current++;
	else
		return;

	presets[current].Activate(device, false);
}

void KeyOverrideCycle::BackEvent(HackerDevice *device)
{
	if (presets.empty())
		return;

	if (smart)
		UpdateCurrent(device);

	if (current == -1)
		current = (int)presets.size() - 1;
	else if (wrap)
		current = (int)(current ? current : presets.size()) - 1;
	else if (current > 0)
		current--;
	else
		return;

	presets[current].Activate(device, false);
}

void KeyOverrideCycleBack::DownEvent(HackerDevice *device)
{
	return cycle->BackEvent(device);
}

// In order to change the iniParams, we need to map them back to system memory so that the CPU
// can change the values, then remap them back to the GPU where they can be accessed by shader code.
// This map/unmap code also requires that the texture be created with the D3D11_USAGE_DYNAMIC flag set.
// This map operation can also cause the GPU to stall, so this should be done as rarely as possible.

static void UpdateIniParams(HackerDevice* wrapper)
{
	ID3D11DeviceContext1* realContext = wrapper->GetPassThroughOrigContext1();
	D3D11_MAPPED_SUBRESOURCE mappedResource;

	if (wrapper->mIniTexture) {
		realContext->Map(wrapper->mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		memcpy(mappedResource.pData, G->iniParams.data(), sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
		realContext->Unmap(wrapper->mIniTexture, 0);
		Profiling::iniparams_updates++;
	}
}

std::vector<CommandList*> pending_post_command_lists;

void Override::Activate(HackerDevice *device, bool override_has_deactivate_condition)
{
	if (is_conditional && condition.evaluate(NULL, device) == 0) {
		LogInfo("Skipping override activation: condition not met\n");
		return;
	}

	LogInfo("User key activation -->\n");
	
	if (override_has_deactivate_condition) {
		active = true;
		OverrideSave.Save(device, this);
	}

	CurrentTransition.ScheduleTransition(device,
			&mOverrideParams,
			&mOverrideVars,
			transition,
			transition_type);

	RunCommandList(device, device->GetHackerContext(), &activate_command_list, NULL, false);
	if (!override_has_deactivate_condition) {
		// type=activate or type=cycle that don't have an explicit deactivate
		// We run their post lists after the upcoming UpdateTransitions() so
		// that they can see the newly set values
		pending_post_command_lists.emplace_back(&deactivate_command_list);
	}
}

void Override::Deactivate(HackerDevice *device)
{
	if (!active) {
		LogInfo("Skipping override deactivation: not active\n");
		return;
	}

	LogInfo("User key deactivation <--\n");

	active = false;
	OverrideSave.Restore(this);

	CurrentTransition.ScheduleTransition(device,
			&mSavedParams,
			&mSavedVars,
			release_transition,
			release_transition_type);

	RunCommandList(device, device->GetHackerContext(), &deactivate_command_list, NULL, true);
}

void Override::Toggle(HackerDevice *device)
{
	if (is_conditional && condition.evaluate(NULL, device) == 0) {
		LogInfo("Skipping toggle override: condition not met\n");
		return;
	}

	if (active)
		return Deactivate(device);
	return Activate(device, true);
}

void KeyOverride::DownEvent(HackerDevice *device)
{
	if (type == KeyOverrideType::TOGGLE)
		return Toggle(device);

	if (type == KeyOverrideType::HOLD && !active)
		return Activate(device, true);

	return Activate(device, false);
}

void KeyOverride::UpEvent(HackerDevice *device)
{
	if (type == KeyOverrideType::HOLD)
		return Deactivate(device);
}

void PresetOverride::Trigger(CommandListCommand *triggered_from)
{
	if (unique_triggers_required) {
		triggers_this_frame.insert(triggered_from);
		if (triggers_this_frame.size() >= unique_triggers_required)
			triggered = true;
	} else {
		triggered = true;
	}
}

void PresetOverride::Exclude()
{
	excluded = true;
}

// Called on present to update the activation status. If the preset was
// triggered this frame it will remain active, otherwise it will deactivate.
void PresetOverride::Update(HackerDevice *wrapper)
{
	if (!active && triggered && !excluded)
		Override::Activate(wrapper, true);
	else if (active && (!triggered || excluded))
		Override::Deactivate(wrapper);

	if (unique_triggers_required)
		triggers_this_frame.clear();
	triggered = false;
	excluded = false;
}

static void _ScheduleTransition(struct OverrideTransitionParam *transition,
		char *name, float current, float val, ULONGLONG now, int time,
		TransitionType transition_type)
{
	LogInfoNoNL(" %s: %#.2g -> %#.2g", name, current, val);
	transition->start = current;
	transition->target = val;
	transition->activation_time = now;
	transition->time = time;
	transition->transition_type = transition_type;
}
// FIXME: Clean up the wide vs sensible string mess and remove this duplicate function:
static void _ScheduleTransition(struct OverrideTransitionParam *transition,
		const wchar_t *name, float current, float val, ULONGLONG now, int time,
		TransitionType transition_type)
{
	LogInfoNoNL(" %S: %#.2g -> %#.2g", name, current, val);
	transition->start = current;
	transition->target = val;
	transition->activation_time = now;
	transition->time = time;
	transition->transition_type = transition_type;
}

void OverrideTransition::ScheduleTransition(HackerDevice *wrapper,
		OverrideParams *targets,
		OverrideVars *var_targets,
		int time, TransitionType transition_type)
{
	ULONGLONG now = GetTickCount64();
	float current;
	char buf[8];
	OverrideParams::iterator i;
	OverrideVars::iterator j;

	LogInfoNoNL(" Override");
	if (time) {
		LogInfoNoNL(" transition: %ims", time);
		LogInfoNoNL(" transition_type: %s",
			lookup_enum_name<const char *, TransitionType>(TransitionTypeNames, transition_type));
	}

	for (i = targets->begin(); i != targets->end(); i++) {
		StringCchPrintfA(buf, 8, "%c%.0i", i->first.chr(), i->first.idx);
		_ScheduleTransition(&params[i->first], buf, G->iniParams[i->first.idx].*i->first.component,
				i->second, now, time, transition_type);
	}
	for (j = var_targets->begin(); j != var_targets->end(); j++) {
		_ScheduleTransition(&vars[j->first], j->first->name.c_str(), j->first->fval,
				j->second, now, time, transition_type);
	}
	LogInfo("\n");
}

void OverrideTransition::UpdatePresets(HackerDevice *wrapper)
{
	PresetOverrideMap::iterator i;

	// Deactivate any presets that were not triggered this frame:
	for (i = presetOverrides.begin(); i != presetOverrides.end(); i++)
		i->second.Update(wrapper);
}

static float _UpdateTransition(struct OverrideTransitionParam *transition, ULONGLONG now)
{
	ULONGLONG time;
	float percent;

	if (transition->time == -1)
		return FLT_MAX;

	if (transition->time == 0) {
		transition->time = -1;
		return transition->target;
	}

	time = now - transition->activation_time;
	percent = (float)time / transition->time;

	if (percent >= 1.0f) {
		transition->time = -1;
		return transition->target;
	}

	if (transition->transition_type == TransitionType::COSINE)
		percent = (float)((1.0 - cos(percent * M_PI)) / 2.0);

	percent = transition->target * percent + transition->start * (1.0f - percent);

	return percent;
}

void OverrideTransition::UpdateTransitions(HackerDevice *wrapper)
{
	std::map<OverrideParam, OverrideTransitionParam>::iterator i;
	std::map<CommandListVariable*, OverrideTransitionParam>::iterator j;
	ULONGLONG now = GetTickCount64();
	float val;

	if (!params.empty()) {
		LogDebugNoNL(" IniParams remapped to ");
		for (i = params.begin(); i != params.end();) {
			float val = _UpdateTransition(&i->second, now);
			G->iniParams[i->first.idx].*i->first.component = val;
			LogDebugNoNL("%c%.0i=%#.2g, ", i->first.chr(), i->first.idx, val);
			if (i->second.time == -1)
				i = params.erase(i);
			else
				i++;
		}
		LogDebug("\n");

		UpdateIniParams(wrapper);
	}

	if (!vars.empty()) {
		LogDebugNoNL(" Variables remapped to ");
		for (j = vars.begin(); j != vars.end();) {
			float val = _UpdateTransition(&j->second, now);
			if (j->first->fval != val) {
				j->first->fval = val;
				if (j->first->flags & VariableFlags::PERSIST)
					G->user_config_dirty |= 1;
			}
			LogDebugNoNL("%S=%#.2g, ", j->first->name.c_str(), val);
			if (j->second.time == -1)
				j = vars.erase(j);
			else
				j++;
		}
		LogDebug("\n");
	}

	// Run any post command lists from type=activate / cycle now so that
	// they can see the first frame of the updated value:
	for (auto i : pending_post_command_lists)
		RunCommandList(wrapper, wrapper->GetHackerContext(), i, NULL, true);
	pending_post_command_lists.clear();
}

void OverrideTransition::Stop()
{
	params.clear();
	vars.clear();
}

OverrideGlobalSaveParam::OverrideGlobalSaveParam() :
	save(FLT_MAX),
	refcount(0)
{
}

float OverrideGlobalSaveParam::Reset()
{
	float ret = save;

	save = FLT_MAX;
	refcount = 0;

	return ret;
}

void OverrideGlobalSave::Reset(HackerDevice* wrapper)
{
	float val;

	params.clear();
	vars.clear();

	// Make sure any current transition won't continue to change the
	// parameters after the reset:
	CurrentTransition.Stop();
}

void OverrideGlobalSaveParam::Save(float val)
{
	if (!refcount)
		save = val;
	refcount++;
}

// Saves the current values for each parameter to the override's local save
// area, and the global save area (if nothing is already saved there).
// If a parameter is currently in a transition, the target value of that
// transition is used instead of the current value. This prevents an
// intermediate transition value from being saved and restored later (e.g. if
// rapidly pressing RMB with a release_transition set).

void OverrideGlobalSave::Save(HackerDevice *wrapper, Override *preset)
{
	OverrideParams::iterator i;
	OverrideVars::iterator j;
	float val;

	for (i = preset->mOverrideParams.begin(); i != preset->mOverrideParams.end(); i++) {
		std::map<OverrideParam, OverrideTransitionParam>::iterator transition = CurrentTransition.params.find(i->first);
		if (transition != CurrentTransition.params.end() && transition->second.time != -1)
			val = transition->second.target;
		else
			val = G->iniParams[i->first.idx].*i->first.component;

		preset->mSavedParams[i->first] = val;
		params[i->first].Save(val);
	}

	for (j = preset->mOverrideVars.begin(); j != preset->mOverrideVars.end(); j++) {
		std::map<CommandListVariable*, OverrideTransitionParam>::iterator transition = CurrentTransition.vars.find(j->first);
		if (transition != CurrentTransition.vars.end() && transition->second.time != -1)
			val = transition->second.target;
		else
			val = j->first->fval;

		preset->mSavedVars[j->first] = val;
		vars[j->first].Save(val);
	}
}

int OverrideGlobalSaveParam::Restore(float *val)
{
	refcount--;

	if (!refcount) {
		if (val)
			*val = save;
		save = FLT_MAX;
	} else if (refcount < 0) {
		LogInfo("BUG! OverrideGlobalSaveParam refcount < 0!\n");
	}

	return refcount;
}

void OverrideGlobalSave::Restore(Override *preset)
{
	OverrideParams::iterator j;
	OverrideVars::iterator l;

	// This replaces the local saved values in the override with the global
	// ones for any parameters where this is the last override being
	// deactivated. This ensures that we will finally restore the original
	// value, even if keys were held and released in a bad order, or a
	// local value was saved in the middle of a transition.

	for (auto next = begin(params), i = next; i != end(params); i = next) {
		next++;
		j = preset->mOverrideParams.find(i->first);
		if (j != preset->mOverrideParams.end()) {
			if (!i->second.Restore(&preset->mSavedParams[i->first])) {
				LogDebug("removed ini param %c%.0i save area\n", i->first.chr(), i->first.idx);
				next = params.erase(i);
			}
		}
	}

	for (auto next = begin(vars), k = next; k != end(vars); k = next) {
		next++;
		l = preset->mOverrideVars.find(k->first);
		if (l != preset->mOverrideVars.end()) {
			if (!k->second.Restore(&preset->mSavedVars[k->first])) {
				LogDebug("removed var %S save area\n", k->first->name.c_str());
				next = vars.erase(k);
			}
		}
	}
}
