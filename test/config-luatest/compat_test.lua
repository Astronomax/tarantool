-- tags: parallel

local fun = require('fun')
local compat = require('compat')
local instance_config = require('internal.config.instance_config')
local t = require('luatest')
local cbuilder = require('luatest.cbuilder')
local cluster = require('test.config-luatest.cluster')

local g = t.group()

g.before_all(cluster.init)
g.after_each(cluster.drop)
g.after_all(cluster.clean)

-- These options can't be changed on reload.
local non_dynamic = {
    box_cfg_replication_sync_timeout = {
        error = "The compat  option 'box_cfg_replication_sync_timeout' " ..
            "takes effect only before the initial box.cfg() call",
    },
}

-- The order of checking is deterministic, so we can only check for one
-- specific error - a conflict with the first dependency in
-- dependencies list.
local dependencies_error_fmt = "The compat option '%s' may be set to 'new' " ..
    "only when compat option '%s' is 'new'"

-- Ensure that all the options from the compat module are present
-- in the compat sections of the instance config and vice versa.
--
-- Also verify that the defaults are the same.
g.test_parity = function()
    -- Mapping option name -> default value.
    local config_options = instance_config:pairs():filter(function(w)
        return w.path[1] == 'compat'
    end):map(function(w)
        assert(#w.path == 2)
        return w.path[2], w.schema.default
    end):tomap()

    -- Mapping option name -> default value.
    local compat_options = fun.iter(compat._options()):map(function(k, v)
        return k, v.default
    end):tomap()

    t.assert_equals(config_options, compat_options)
end

-- Verify that the given compat option has the given effective
-- value.
local function verify(cluster, option_name, exp)
    if exp == 'default' then
        exp = compat._options()[option_name].default
    end

    cluster['instance-001']:exec(function(option_name, exp)
        local compat = require('compat')

        local res = compat[option_name]:is_new() and 'new' or 'old'
        t.assert_equals(res, exp)
    end, {option_name, exp})
end

local function gen_reload_config(option_name, v, ignore_dependencies)
    local actual_value = v ~= 'default'
        and v or compat[option_name].default

    if not ignore_dependencies then
        -- Build a valid config that satisfies all dependencies
        local compat_config = { [option_name] = actual_value, }
        local pending = { option_name, }
        while #pending > 0 do
            local option = compat[pending[1]]
            -- 'new' value can only conflict with dependencies, and 'old' value
            -- can only conflict with dependents (If an option is set to 'new',
            -- all its dependencies must be set to 'new' and vise versa)
            local dependencies = (actual_value == 'new') and
                option.dependencies or option.dependents
            for _, neighbour_name in ipairs(dependencies) do
                if compat_config[neighbour_name] == nil then
                    table.insert(pending, neighbour_name)
                    compat_config[neighbour_name] = actual_value
                end
            end
            table.remove(pending, 1)
        end
        return compat_config
    else
        return { [option_name] = v ~= 'default' and v or box.NULL, }
    end
end

local function gen_startup_config(option_name, v, ignore_dependencies)
    return fun.iter(gen_reload_config(option_name, v, ignore_dependencies))
        :filter(function(_, v) return v ~= box.NULL end):tomap()
end

-- Write a new config file with the given compat option and reload
-- the configuration.
local function switch(cluster, option_name, startup_config, v,
    ignore_dependencies)
    local config = cbuilder:new(startup_config)
        :set_instance_option('instance-001', 'compat',
            gen_reload_config(option_name, v, ignore_dependencies))
        :config()
    cluster:reload(config)
end

-- Start an instance with the given compat option value and verify
-- that it is actually set.
local function gen_startup_case(option_name, v)
    return function(g)
        local startup_config = cbuilder:new()
            :add_instance('instance-001', { compat =
                gen_startup_config(option_name, v), })
            :config()

        local cluster = cluster.new(g, startup_config)
        cluster:start()

        verify(cluster, option_name, v)
    end
end

-- Start an instance and try to switch the given compat option
-- with a reload and a verification of the effect.
local function gen_reload_case(option_name)
    return function(g)
        local startup_config = cbuilder:new()
            :add_instance('instance-001', {})
            :config()

        local cluster = cluster.new(g, startup_config)
        cluster:start()

        verify(cluster, option_name, 'default')

        switch(cluster, option_name, startup_config, 'old')
        verify(cluster, option_name, 'old')

        switch(cluster, option_name, startup_config, 'new')
        verify(cluster, option_name, 'new')

        switch(cluster, option_name, startup_config, 'default')
        verify(cluster, option_name, 'default')
    end
end

-- Verify that an attempt to change the option on reload leads to
-- an error.
local function gen_reload_failure_case(option_name, startup_value, reload_value,
    exp_err, ignore_dependencies)
    return function(g)
        local startup_config = cbuilder:new()
            :add_instance('instance-001', { compat =
                gen_startup_config(option_name, startup_value), })
            :config()

        local cluster = cluster.new(g, startup_config)
        cluster:start()

        -- Attempt to switch the option to another value.
        t.assert_error_msg_content_equals(exp_err, switch, cluster,
            option_name, startup_config, reload_value, ignore_dependencies)

        -- Verify that the value remains the same.
        verify(cluster, option_name, startup_value)

        -- Verify that config module reports an error.
        cluster['instance-001']:exec(function(exp_err)
            local config = require('config')

            local info = config:info()
            t.assert_equals({
                status = info.status,
                alert_count = #info.alerts,
                alert_1 = {
                    type = 'error',
                    message = info.alerts[1].message:gsub('^.-:[0-9]+: ', ''),
                },
            }, {
                status = 'check_errors',
                alert_count = 1,
                alert_1 = {
                    type = 'error',
                    message = exp_err,
                },
            })
        end, {exp_err})

        -- Verify that things can be repaired using config:reload()
        switch(cluster, option_name, startup_config, startup_value)
        cluster['instance-001']:exec(function()
            local config = require('config')

            local info = config:info()
            t.assert_equals({
                status = info.status,
                alerts = info.alerts,
            }, {
                status = 'ready',
                alerts = {},
            })
        end)
    end
end

-- Verify that start with the given compat option value and reload
-- it to the second given value is successful.
local function gen_reload_success_case(option_name, startup_value, reload_value)
    return function(g)
        local startup_config = cbuilder:new()
            :add_instance('instance-001', { compat =
                gen_startup_config(option_name, startup_value), })
            :config()

        local cluster = cluster.new(g, startup_config)
        cluster:start()

        -- Verify that the option is switched successfully.
        switch(cluster, option_name, startup_config, reload_value)
        verify(cluster, option_name, reload_value)

        -- Verify that config module doesn't report an error.
        cluster['instance-001']:exec(function()
            local config = require('config')

            local info = config:info()
            t.assert_equals({
                status = info.status,
                alerts = info.alerts,
            }, {
                status = 'ready',
                alerts = {},
            })
        end)
    end
end

-- Walk over the compat options and generate test cases for them.
for option_name, _ in pairs(compat._options()) do
    -- Start with the given option value.
    local case_name_prefix = ('test_startup_%s_'):format(option_name)
    for _, v in ipairs({'default', 'old', 'new'}) do
        g[case_name_prefix .. v] = gen_startup_case(option_name, v)
    end

    -- An option with dependencies cannot be set to 'new' until all its
    -- dependencies are set to new.
    local dependencies = compat[option_name].dependencies
    if #dependencies > 0 then
        local case_name = ('test_reload_%s_failure_unsatisfied_dependencies')
            :format(option_name)
        local exp_err = dependencies_error_fmt
            :format(option_name, dependencies[1])
        g[case_name] = gen_reload_failure_case(
            option_name, 'old', 'new', exp_err, true)
    end

    if non_dynamic[option_name] == nil then
        -- Start without compat option and set it on reload.
        g['test_reload_' .. option_name] = gen_reload_case(option_name)
    else
        -- Start with one option name and set another on reload (expect an
        -- error).
        --
        -- Also verify that null and an effective default may be
        -- used interchangeably and switching from one to another
        -- on reload doesn't trigger any error.
        local case_name_prefix = ('test_reload_%s_'):format(option_name)
        local exp_err = non_dynamic[option_name].error
        g[case_name_prefix .. 'failure_old_to_new'] = gen_reload_failure_case(
            option_name, 'old', 'new', exp_err)
        g[case_name_prefix .. 'failure_new_to_old'] = gen_reload_failure_case(
            option_name, 'new', 'old', exp_err)
        local def = compat._options()[option_name].default
        if def == 'old' then
            g[case_name_prefix .. 'failure_default_to_new'] =
                gen_reload_failure_case(option_name, 'default', 'new', exp_err)
            g[case_name_prefix .. 'failure_new_to_default'] =
                gen_reload_failure_case(option_name, 'new', 'default', exp_err)
            g[case_name_prefix .. 'success_default_to_old'] =
                gen_reload_success_case(option_name, 'default', 'old')
            g[case_name_prefix .. 'success_old_to_default'] =
                gen_reload_success_case(option_name, 'old', 'default')
        else
            g[case_name_prefix .. 'success_default_to_new'] =
                gen_reload_success_case(option_name, 'default', 'new')
            g[case_name_prefix .. 'success_new_to_default'] =
                gen_reload_success_case(option_name, 'new', 'default')
            g[case_name_prefix .. 'failure_default_to_old'] =
                gen_reload_failure_case(option_name, 'default', 'old', exp_err)
            g[case_name_prefix .. 'failure_old_to_default'] =
                gen_reload_failure_case(option_name, 'old', 'default', exp_err)
        end
    end
end
