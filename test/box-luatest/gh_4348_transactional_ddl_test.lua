local server = require('luatest.server')
local t = require('luatest')

local g = t.group('gh-4348-transactional-ddl-test', {
    {mvcc = false, engine = 'memtx'},
    {mvcc = false, engine = 'vinyl'},
    {mvcc = true, engine = 'memtx'},
    {mvcc = true, engine = 'vinyl'},
})

g.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {memtx_use_mvcc_engine = cg.params.mvcc}
    })
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.after_each(function(cg)
    cg.server:exec(function()
        if box.space.s ~= nil then
            box.space.s:drop()
        end
        if box.space.s_renamed ~= nil then
            box.space.s_renamed:drop()
        end
        if box.space.fs1 ~= nil then
            box.space.fs1:drop()
        end
        if box.space.fs2 ~= nil then
            box.space.fs2:drop()
        end
        if box.space.fs3 ~= nil then
            box.space.fs3:drop()
        end
        if box.func.constr1 ~= nil then
            box.func.constr1:drop()
        end
        if box.func.constr2 ~= nil then
            box.func.constr2:drop()
        end
    end)
end)

g.test_transactional_ddl = function(cg)
    cg.server:exec(function(engine)
        ------------------------------------------------------------------------
        -- box.schema.space.create ---------------------------------------------
        ------------------------------------------------------------------------

        local value
        local s = box.schema.space.create('s', {engine = engine})
        local fs1 = box.schema.space.create('fs1', {engine = engine})
        local fs2 = box.schema.space.create('fs2', {engine = engine})
        local fs3 = box.schema.space.create('fs3', {engine = engine})

        s:create_index('spk', {parts = {{1, 'scalar'}}})
        fs1:create_index('fs1pk', {parts = {{1, 'scalar'}}})
        fs2:create_index('fs2pk', {parts = {{1, 'scalar'}}})
        fs3:create_index('fs3pk', {parts = {{1, 'scalar'}}})

        s:create_index('iut', {
            parts = {{1, 'scalar'}},
            unique = true,
            type = 'tree'
        })

        s:create_index('int', {
            parts = {{1, 'scalar'}},
            unique = false,
            type = 'tree'
        })

        if engine ~= 'vinyl' then
            s:create_index('iuh', {
                parts = {{1, 'scalar'}},
                unique = true,
                type = 'hash'
            })
        end

        local fs1sk = fs1:create_index('fs1sk', {
            parts = {{1, 'unsigned'}},
            unique = false
        })

        box.schema.func.create('constr1', {
            language = 'LUA',
            is_deterministic = true,
            body = 'function(t, c) return t.id < 999999 end'
        })

        box.schema.func.create('constr2', {
            language = 'LUA',
            is_deterministic = true,
            body = 'function(t, c) return t.id < 99999 end'
        })

        -- Fill the spaces.
        for i = 0, 1001  do -- More than 1k to ensure yielding.
            fs1:insert({i + 1})
            fs2:insert({i + 2})
            fs3:insert({i + 3})
            s:insert({i, i + 1, i + 2, i + 3})
        end

        ------------------------------------------------------------------------
        -- box.schema.space.format ---------------------------------------------
        ------------------------------------------------------------------------

        -- Non-yielding format change.
        value = {{name = 'id', type = 'scalar'}}
        box.schema.space.format(s.id, value)
        t.assert_equals(box.schema.space.format(s.id), value)

        -- Yielding format change.
        value = {{name = 'id', type = 'number'}}
        box.schema.space.format(s.id, value)
        t.assert_equals(box.schema.space.format(s.id), value)

        ------------------------------------------------------------------------
        -- box.schema.space.rename ---------------------------------------------
        ------------------------------------------------------------------------

        box.schema.space.rename(s.id, 's_renamed')
        t.assert_equals(s.name, 's_renamed')

        box.schema.space.rename(s.id, 's')
        t.assert_equals(s.name, 's')

        ------------------------------------------------------------------------
        -- box.schema.space.alter ----------------------------------------------
        ------------------------------------------------------------------------

        -- Change field count (strengthen and relax).
        box.schema.space.alter(s.id, {field_count = 4})
        box.schema.space.alter(s.id, {field_count = 0})

        -- Change format.
        value = {
            {name = 'id', type = 'scalar'},
            {name = 'f1', type = 'scalar'},
            {
                name = 'f2',
                type = 'scalar',
                foreign_key = {f2 = {space = fs2.name, field = 1}}
            },
            {name = 'f3', type = 'scalar'},
        }
        box.schema.space.alter(s.id, {format = value})

        -- Change sync flag.
        box.schema.space.alter(s.id, {is_sync = true})
        t.assert_equals(s.is_sync, true)

        box.schema.space.alter(s.id, {is_sync = false})
        t.assert_equals(s.is_sync, false)

        -- Change defer_deletes flag.
        box.schema.space.alter(s.id, {defer_deletes = true})
        box.schema.space.alter(s.id, {defer_deletes = false})

        -- Change name.
        box.schema.space.alter(s.id, {name = 's_renamed'})
        t.assert_equals(s.name, 's_renamed')

        box.schema.space.alter(s.id, {name = 's'})
        t.assert_equals(s.name, 's')

        -- Change constraint.
        box.schema.space.alter(s.id, {constraint = 'constr1'})

        -- Change foreigin key.
        value = {
            f1 = {space = fs1.name, field = {f1 = 1}}
        }
        box.schema.space.alter(s.id, {foreign_key = value})

        -- Change everything at once.
        value = {
            field_count = 4,
            format = {
                {name = 'id', type = 'number'},
                {name = 'f1', type = 'number'},
                {name = 'f2', type = 'number'},
                {name = 'f3', type = 'number'},
            },
            is_sync = true,
            defer_deletes = true,
            name = 's_renamed',
            constraint = 'constr2',
            foreign_key = {
                f1 = {space = fs1.name, field = {f1 = 1}},
                f2 = {space = fs2.name, field = {f2 = 1}},
                f3 = {space = fs3.name, field = {f3 = 1}},
            }
        }
        box.schema.space.alter(s.id, value)

        ------------------------------------------------------------------------
        -- box.schema.space.drop -----------------------------------------------
        ------------------------------------------------------------------------

        -- Attempt tp drop a referenced foreign key space with secondary
        -- indexes.
        --
        -- Currently the space drop flow looks like this:
        -- 1. Drop automatically generated sequence for the space.
        -- 2. Drop triggers of the space.
        -- 3. Disable functional indexes of the space.
        -- 4. (!) Remove each index of the space starting from secondary
        --    indexes.
        -- 5. Revoke the space privileges.
        -- 6. Remove the associated entry from the _truncate system space.
        -- 7. Remove the space entry from _space system space.
        --
        -- If the space is referenced by another space with foreign key
        -- constraint then the flow fails on the primary index drop (step 4).
        -- But at that point all the secondary indexes are dropped already, so
        -- we have an inconsistent state of the database.
        --
        -- But if the drop function is transactional then the dropped secondary
        -- indexes are restored on transaction revert and the database remains
        -- consistent: we can continue using the secondary index of the table we
        -- have failed to drop.

        local err = "Can't modify space '" .. fs1.name
                    .. "': space is referenced by foreign key"
        t.assert_error_msg_equals(err, fs1.drop, fs1)

        -- The secondary index is restored on drop fail so this must succeed.
        fs1sk:select(42)

        ------------------------------------------------------------------------
        -- box.schema.index.rename ---------------------------------------------
        ------------------------------------------------------------------------

        local new_name = 'fs1sk_renamed'

        box.schema.index.rename(fs1.id, fs1sk.id, new_name)
        -- FIXME: On the secondary index drop its lua object was invalidated,
        -- so it does not hold the new index name, and we have to restore it
        -- from fs1.index[new_name].
        --
        -- Please remove this comment and the following three lines once the
        -- issue is solved.
        t.assert_not_equals(fs1sk.name, new_name)
        t.assert_not_equals(fs1.index[new_name], nil)
        fs1sk = fs1.index[new_name]
        t.assert_equals(fs1sk.name, new_name)

        ------------------------------------------------------------------------
        -- box.schema.index.create/drop ----------------------------------------
        ------------------------------------------------------------------------

        local all_parts = {
            {1, 'number'}, {2, 'number'}, {3, 'number'}, {4, 'number'}
        }
        local one_part = {{1, 'number'}}

        local indexes = {
            {type = 'tree', parts = one_part, unique = true},
            {type = 'tree', parts = one_part, unique = false},
            {type = 'tree', parts = all_parts, unique = true},
            {type = 'tree', parts = all_parts, unique = false},
        }

        if engine ~= 'vinyl' then
            table.insert(indexes, {type = 'hash', parts = one_part})
            table.insert(indexes, {type = 'hash', parts = all_parts})
        end

        for _, index in pairs(indexes) do
            local tmp_idx = box.schema.index.create(s.id, 'tmp_idx', index)
            box.schema.index.drop(s.id, tmp_idx.id)
        end
    end, {cg.params.engine})
end
