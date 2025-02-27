/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "naxxramas.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "TemporarySummon.h"

enum Yells
{
    EMOTE_AIR_PHASE         = 0,
    EMOTE_GROUND_PHASE      = 1,
    EMOTE_BREATH            = 2,
    EMOTE_ENRAGE            = 3
};

enum Spells
{
    SPELL_FROST_AURA                    = 28531,
    SPELL_CLEAVE                        = 19983,
    SPELL_TAIL_SWEEP                    = 55697,
    SPELL_SUMMON_BLIZZARD               = 28560,
    SPELL_LIFE_DRAIN                    = 28542,
    SPELL_ICEBOLT                       = 28522,
    SPELL_FROST_BREATH_ANTICHEAT        = 29318, // damage effect ignoring LoS on the entrance platform to prevent cheese
    SPELL_FROST_BREATH                  = 28524, // damage effect below sapphiron
    SPELL_FROST_MISSILE                 = 30101, // visual only
    SPELL_BERSERK                       = 26662,
    SPELL_DIES                          = 29357,
    SPELL_CHECK_RESISTS                 = 60539,
    SPELL_SUMMON_WING_BUFFET            = 29329,
    SPELL_WING_BUFFET_PERIODIC          = 29327,
    SPELL_WING_BUFFET_DESPAWN_PERIODIC  = 29330,
    SPELL_DESPAWN_BUFFET                = 29336
};

enum Phases
{
    PHASE_BIRTH = 1,
    PHASE_GROUND,
    PHASE_FLIGHT
};

enum Events
{
    EVENT_BERSERK       = 1,
    EVENT_CLEAVE,
    EVENT_TAIL,
    EVENT_DRAIN,
    EVENT_BLIZZARD,
    EVENT_FLIGHT,
    EVENT_LIFTOFF,
    EVENT_ICEBOLT,
    EVENT_BREATH,
    EVENT_EXPLOSION,
    EVENT_LAND,
    EVENT_GROUND,
    EVENT_BIRTH,
    EVENT_CHECK_RESISTS
};

enum Misc
{
    NPC_BLIZZARD            = 16474,
    GO_ICEBLOCK             = 181247,

    // The Hundred Club
    DATA_THE_HUNDRED_CLUB   = 21462147,
    MAX_FROST_RESISTANCE    = 100,
    ACTION_BIRTH            = 1,
    DATA_BLIZZARD_TARGET
};

typedef std::map<ObjectGuid, ObjectGuid> IceBlockMap;

class BlizzardTargetSelector
{
public:
    BlizzardTargetSelector(std::vector<Unit*> const& blizzards) : _blizzards(blizzards) { }

    bool operator()(Unit* unit) const
    {
        if (unit->GetTypeId() != TYPEID_PLAYER)
            return false;

        // Check if unit is target of some blizzard
        for (Unit* blizzard : _blizzards)
            if (blizzard->GetAI()->GetGUID(DATA_BLIZZARD_TARGET) == unit->GetGUID())
                return false;

        return true;
    }

private:
    std::vector<Unit*> const& _blizzards;
};

struct boss_sapphiron : public BossAI
{
    boss_sapphiron(Creature* creature) :
        BossAI(creature, BOSS_SAPPHIRON)
    {
        Initialize();
    }

    void Initialize()
    {
        _delayedDrain = false;
        _canTheHundredClub = true;
    }

    void InitializeAI() override
    {
        if (instance->GetBossState(BOSS_SAPPHIRON) == DONE)
            return;

        _canTheHundredClub = true;

        if (!instance->GetData(DATA_HAD_SAPPHIRON_BIRTH))
        {
            me->SetVisible(false);
            me->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
            me->SetReactState(REACT_PASSIVE);
        }

        BossAI::InitializeAI();
    }

    void Reset() override
    {
        if (events.IsInPhase(PHASE_FLIGHT))
        {
            instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_ICEBOLT, true, true);
            me->SetReactState(REACT_AGGRESSIVE);
            if (me->IsHovering())
            {
                me->HandleEmoteCommand(EMOTE_ONESHOT_LAND);
                me->SetHover(false);
            }
        }

        _Reset();
        Initialize();
    }

    void DamageTaken(Unit* /*who*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo = nullptr*/) override
    {
        if (damage < me->GetHealth() || !events.IsInPhase(PHASE_FLIGHT))
            return;
        damage = me->GetHealth()-1; // don't die during air phase
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);

        me->CastSpell(me, SPELL_FROST_AURA, true);

        events.SetPhase(PHASE_GROUND);
        events.ScheduleEvent(EVENT_CHECK_RESISTS, 0s);
        events.ScheduleEvent(EVENT_BERSERK, 15min);
        EnterPhaseGround(true);
    }

    void SpellHitTarget(WorldObject* target, SpellInfo const* spellInfo) override
    {
        Unit* unitTarget = target->ToUnit();
        if (!unitTarget)
            return;

        switch(spellInfo->Id)
        {
            case SPELL_CHECK_RESISTS:
                if (unitTarget->GetResistance(SPELL_SCHOOL_FROST) > MAX_FROST_RESISTANCE)
                    _canTheHundredClub = false;
                break;
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        _JustDied();
        me->CastSpell(me, SPELL_DIES, true);
    }

    void MovementInform(uint32 /*type*/, uint32 id) override
    {
        if (id == 1)
            events.ScheduleEvent(EVENT_LIFTOFF, 0s, 0, PHASE_FLIGHT);
    }

    void DoAction(int32 param) override
    {
        if (param == ACTION_BIRTH)
        {
            events.SetPhase(PHASE_BIRTH);
            events.ScheduleEvent(EVENT_BIRTH, 23s);
        }
    }

    void EnterPhaseGround(bool initial)
    {
        me->SetReactState(REACT_AGGRESSIVE);
        events.ScheduleEvent(EVENT_CLEAVE, randtime(Seconds(5), Seconds(15)), 0, PHASE_GROUND);
        events.ScheduleEvent(EVENT_TAIL, randtime(Seconds(7), Seconds(10)), 0, PHASE_GROUND);
        events.ScheduleEvent(EVENT_BLIZZARD, randtime(Seconds(5), Seconds(10)), 0, PHASE_GROUND);
        if (initial)
        {
            events.ScheduleEvent(EVENT_DRAIN, randtime(Seconds(22), Seconds(28)));
            events.ScheduleEvent(EVENT_FLIGHT, Seconds(48) + Milliseconds(500), 0, PHASE_GROUND);
        }
        else
            events.ScheduleEvent(EVENT_FLIGHT, Minutes(1), 0, PHASE_GROUND);
    }

    inline void CastDrain()
    {
        DoCastAOE(SPELL_LIFE_DRAIN);
        events.ScheduleEvent(EVENT_DRAIN, randtime(Seconds(22), Seconds(28)));
    }

    uint32 GetData(uint32 data) const override
    {
        if (data == DATA_THE_HUNDRED_CLUB)
            return _canTheHundredClub;

        return 0;
    }

    ObjectGuid GetGUID(int32 data) const override
    {
        if (data == DATA_BLIZZARD_TARGET)
        {
            // Filtering blizzards from summon list
            std::vector<Unit*> blizzards;
            for (ObjectGuid summonGuid : summons)
                if (summonGuid.GetEntry() == NPC_BLIZZARD)
                    if (Unit* temp = ObjectAccessor::GetUnit(*me, summonGuid))
                        blizzards.push_back(temp);

            if (Unit* newTarget = me->AI()->SelectTarget(SelectTargetMethod::Random, 1, BlizzardTargetSelector(blizzards)))
                return newTarget->GetGUID();
        }

        return ObjectGuid::Empty;
    }

    void UpdateAI(uint32 diff) override
    {
        events.Update(diff);

        if (!events.IsInPhase(PHASE_BIRTH) && !UpdateVictim())
            return;

        if (events.IsInPhase(PHASE_GROUND))
        {
            while (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_CHECK_RESISTS:
                        DoCast(me, SPELL_CHECK_RESISTS);
                        events.Repeat(Seconds(30));
                        return;
                    case EVENT_GROUND:
                        EnterPhaseGround(false);
                        return;
                    case EVENT_BERSERK:
                        Talk(EMOTE_ENRAGE);
                        DoCast(me, SPELL_BERSERK);
                        return;
                    case EVENT_CLEAVE:
                        DoCastVictim(SPELL_CLEAVE);
                        events.ScheduleEvent(EVENT_CLEAVE, randtime(Seconds(5), Seconds(15)), 0, PHASE_GROUND);
                        return;
                    case EVENT_TAIL:
                        DoCastAOE(SPELL_TAIL_SWEEP);
                        events.ScheduleEvent(EVENT_TAIL, randtime(Seconds(7), Seconds(10)), 0, PHASE_GROUND);
                        return;
                    case EVENT_DRAIN:
                        CastDrain();
                        return;
                    case EVENT_BLIZZARD:
                        DoCastAOE(SPELL_SUMMON_BLIZZARD);
                        events.ScheduleEvent(EVENT_BLIZZARD, RAID_MODE(Seconds(20), Seconds(7)), 0, PHASE_GROUND);
                        break;
                    case EVENT_FLIGHT:
                        if (HealthAbovePct(10))
                        {
                            _delayedDrain = false;
                            events.SetPhase(PHASE_FLIGHT);
                            me->SetReactState(REACT_PASSIVE);
                            me->AttackStop();
                            float x, y, z, o;
                            me->GetHomePosition(x, y, z, o);
                            me->GetMotionMaster()->MovePoint(1, x, y, z);
                            return;
                        }
                        break;
                }
            }

            DoMeleeAttackIfReady();
        }
        else
        {
            if (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_CHECK_RESISTS:
                        DoCast(me, SPELL_CHECK_RESISTS);
                        events.Repeat(Seconds(30));
                        return;
                    case EVENT_LIFTOFF:
                    {
                        Talk(EMOTE_AIR_PHASE);
                        DoCastSelf(SPELL_SUMMON_WING_BUFFET);
                        me->HandleEmoteCommand(EMOTE_ONESHOT_LIFTOFF);
                        me->SetHover(true);
                        events.ScheduleEvent(EVENT_ICEBOLT, Seconds(7), 0, PHASE_FLIGHT);

                        _iceboltTargets.clear();
                        std::list<Unit*> targets;
                        SelectTargetList(targets, RAID_MODE(2, 3), SelectTargetMethod::Random, 0, 200.0f, true);
                        for (Unit* target : targets)
                            _iceboltTargets.push_back(target->GetGUID());
                        return;
                    }
                    case EVENT_ICEBOLT:
                    {
                        if (_iceboltTargets.empty())
                        {
                            events.ScheduleEvent(EVENT_BREATH, Seconds(2), 0, PHASE_FLIGHT);
                            return;
                        }
                        ObjectGuid target = _iceboltTargets.back();
                        if (Player* pTarget = ObjectAccessor::GetPlayer(*me, target))
                            if (pTarget->IsAlive())
                                DoCast(pTarget, SPELL_ICEBOLT);
                        _iceboltTargets.pop_back();

                        if (_iceboltTargets.empty())
                            events.ScheduleEvent(EVENT_BREATH, Seconds(2), 0, PHASE_FLIGHT);
                        else
                            events.Repeat(Seconds(3));
                        return;
                    }
                    case EVENT_BREATH:
                    {
                        Talk(EMOTE_BREATH);
                        DoCastAOE(SPELL_FROST_MISSILE);
                        events.ScheduleEvent(EVENT_EXPLOSION, Seconds(8), 0, PHASE_FLIGHT);
                        return;
                    }
                    case EVENT_EXPLOSION:
                        DoCastAOE(SPELL_FROST_BREATH);
                        DoCastAOE(SPELL_FROST_BREATH_ANTICHEAT);
                        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_ICEBOLT, true, true);
                        events.ScheduleEvent(EVENT_LAND, Seconds(3) + Milliseconds(500), 0, PHASE_FLIGHT);
                        return;
                    case EVENT_LAND:
                        DoCastSelf(SPELL_DESPAWN_BUFFET); /// @todo: at this point it should already despawn, probably that spell is used in another place
                        if (_delayedDrain)
                            CastDrain();
                        me->HandleEmoteCommand(EMOTE_ONESHOT_LAND);
                        Talk(EMOTE_GROUND_PHASE);
                        me->SetHover(false);
                        events.SetPhase(PHASE_GROUND);
                        events.ScheduleEvent(EVENT_GROUND, Seconds(3) + Milliseconds(500), 0, PHASE_GROUND);
                        return;
                    case EVENT_BIRTH:
                        me->SetVisible(true);
                        me->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE);
                        me->SetReactState(REACT_AGGRESSIVE);
                        return;
                    case EVENT_DRAIN:
                        _delayedDrain = true;
                        break;
                }
            }
        }
    }

private:
    GuidVector _iceboltTargets;
    bool _delayedDrain;
    bool _canTheHundredClub;
};

struct npc_sapphiron_blizzard : public ScriptedAI
{
    npc_sapphiron_blizzard(Creature* creature) : ScriptedAI(creature) { }

    void Reset() override
    {
        me->SetReactState(REACT_PASSIVE);
        _scheduler.Schedule(Seconds(3), [this](TaskContext chill)
        {
            DoCastSelf(me->m_spells[0], true);
            chill.Repeat();
        });
    }

    ObjectGuid GetGUID(int32 data) const override
    {
        return data == DATA_BLIZZARD_TARGET ? _targetGuid : ObjectGuid::Empty;
    }

    void SetGUID(ObjectGuid const& guid, int32 id) override
    {
        if (id == DATA_BLIZZARD_TARGET)
            _targetGuid = guid;
    }

    void UpdateAI(uint32 diff) override
    {
        _scheduler.Update(diff);
    }

private:
    TaskScheduler _scheduler;
    ObjectGuid _targetGuid;
};

struct npc_sapphiron_wing_buffet : public ScriptedAI
{
    npc_sapphiron_wing_buffet(Creature* creature) : ScriptedAI(creature) { }

    void InitializeAI() override
    {
        me->SetReactState(REACT_PASSIVE);
    }

    void JustAppeared() override
    {
        DoCastSelf(SPELL_WING_BUFFET_PERIODIC);
        DoCastSelf(SPELL_WING_BUFFET_DESPAWN_PERIODIC);
    }
};

struct go_sapphiron_birth : public GameObjectAI
{
    go_sapphiron_birth(GameObject* go) : GameObjectAI(go), instance(go->GetInstanceScript()) { }

    void OnLootStateChanged(uint32 state, Unit* who) override
    {
        if (state == GO_ACTIVATED)
        {
            if (who)
            {
                if (Creature* sapphiron = ObjectAccessor::GetCreature(*me, instance->GetGuidData(DATA_SAPPHIRON)))
                    sapphiron->AI()->DoAction(ACTION_BIRTH);
                instance->SetData(DATA_HAD_SAPPHIRON_BIRTH, 1u);
            }
        }
        else if (state == GO_JUST_DEACTIVATED)
        { // prevent ourselves from going back to _READY and resetting the client anim
            me->SetRespawnTime(0);
            me->Delete();
        }
    }

    InstanceScript* instance;
};

// 24780 - Dream Fog
class spell_sapphiron_change_blizzard_target : public AuraScript
{
    void HandlePeriodic(AuraEffect const* /*eff*/)
    {
        TempSummon* me = GetTarget()->ToTempSummon();
        if (Creature* owner = me ? me->GetSummonerCreatureBase() : nullptr)
        {
            me->GetAI()->SetGUID(ObjectGuid::Empty, DATA_BLIZZARD_TARGET);
            if (Unit* newTarget = ObjectAccessor::GetUnit(*owner, owner->AI()->GetGUID(DATA_BLIZZARD_TARGET)))
            {
                me->GetAI()->SetGUID(newTarget->GetGUID(), DATA_BLIZZARD_TARGET);
                me->GetMotionMaster()->MoveFollow(newTarget, 0.1f, 0.0f);
            }
            else
            {
                me->StopMoving();
                me->GetMotionMaster()->Clear();
            }
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_sapphiron_change_blizzard_target::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 28522 - Icebolt
class spell_sapphiron_icebolt : public AuraScript
{
    void HandleApply(AuraEffect const* /*eff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->ApplySpellImmune(SPELL_ICEBOLT, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_FROST, true);
    }

    void HandleRemove(AuraEffect const* /*eff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!_block.IsEmpty())
            if (GameObject* oBlock = ObjectAccessor::GetGameObject(*GetTarget(), _block))
                oBlock->Delete();
        GetTarget()->ApplySpellImmune(SPELL_ICEBOLT, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_FROST, false);
    }

    void HandlePeriodic(AuraEffect const* /*eff*/)
    {
        if (!_block.IsEmpty())
            return;
        if (GetTarget()->isMoving())
            return;
        float x, y, z;
        GetTarget()->GetPosition(x, y, z);
        if (GameObject* block = GetTarget()->SummonGameObject(GO_ICEBLOCK, x, y, z, 0.f, QuaternionData(), 25s))
            _block = block->GetGUID();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_sapphiron_icebolt::HandleApply, EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_sapphiron_icebolt::HandleRemove, EFFECT_0, SPELL_AURA_MOD_STUN, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_sapphiron_icebolt::HandlePeriodic, EFFECT_2, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }

    ObjectGuid _block;
};

// 28560 - Summon Blizzard
class spell_sapphiron_summon_blizzard : public SpellScript
{
    bool Validate(SpellInfo const* /*spell*/) override
    {
        return ValidateSpellInfo({ SPELL_SUMMON_BLIZZARD });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        if (Unit* target = GetHitUnit())
            if (Creature* blizzard = GetCaster()->SummonCreature(NPC_BLIZZARD, *target, TEMPSUMMON_TIMED_DESPAWN, randtime(25s, 30s)))
            {
                blizzard->CastSpell(nullptr, blizzard->m_spells[0], TRIGGERED_NONE);
                if (Creature* creatureCaster = GetCaster()->ToCreature())
                {
                    blizzard->AI()->SetGUID(ObjectGuid::Empty, DATA_BLIZZARD_TARGET);
                    if (Unit* newTarget = ObjectAccessor::GetUnit(*creatureCaster, creatureCaster->AI()->GetGUID(DATA_BLIZZARD_TARGET)))
                    {
                        blizzard->AI()->SetGUID(newTarget->GetGUID(), DATA_BLIZZARD_TARGET);
                        blizzard->GetMotionMaster()->MoveFollow(newTarget, 0.1f, 0.0f);
                        return;
                    }
                }
                blizzard->GetMotionMaster()->MoveFollow(target, 0.1f, 0.0f);
            }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_sapphiron_summon_blizzard::HandleDummy, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// 29330 - Sapphiron's Wing Buffet Despawn
class spell_sapphiron_wing_buffet_despawn_periodic : public AuraScript
{
    void PeriodicTick(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (Creature* creature = target->ToCreature())
            creature->DespawnOrUnsummon();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_sapphiron_wing_buffet_despawn_periodic::PeriodicTick, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// 29336 - Despawn Buffet
class spell_sapphiron_despawn_buffet : public SpellScript
{
    void HandleScriptEffect(SpellEffIndex /* effIndex */)
    {
        if (Creature* target = GetHitCreature())
            target->DespawnOrUnsummon();
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_sapphiron_despawn_buffet::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

class achievement_the_hundred_club : public AchievementCriteriaScript
{
    public:
        achievement_the_hundred_club() : AchievementCriteriaScript("achievement_the_hundred_club") { }

        bool OnCheck(Player* /*source*/, Unit* target) override
        {
            return target && target->GetAI()->GetData(DATA_THE_HUNDRED_CLUB);
        }
};

void AddSC_boss_sapphiron()
{
    RegisterNaxxramasCreatureAI(boss_sapphiron);
    RegisterNaxxramasCreatureAI(npc_sapphiron_blizzard);
    RegisterNaxxramasCreatureAI(npc_sapphiron_wing_buffet);
    RegisterNaxxramasGameObjectAI(go_sapphiron_birth);
    RegisterSpellScript(spell_sapphiron_change_blizzard_target);
    RegisterSpellScript(spell_sapphiron_icebolt);
    RegisterSpellScript(spell_sapphiron_summon_blizzard);
    RegisterSpellScript(spell_sapphiron_wing_buffet_despawn_periodic);
    RegisterSpellScript(spell_sapphiron_despawn_buffet);
    new achievement_the_hundred_club();
}
