nilyahin_tribunal = Creature:new {
 customName = "Ni'lyahin Tribunal",
 randomNameType = NAME_GENERIC,
	socialGroup = "nilyahin_smugglers",
	faction = "",
	level = 300,
	chanceHit = 19,
	damageMin = 1245,
	damageMax = 2200,
	baseXp = 20948,
	baseHAM = 350000,
	baseHAMmax = 350000,
	armor = 3,
	resists = {80,95,80,95,95,95,80,95,185},
	meatType = "",
	meatAmount = 0,
	hideType = "",
	hideAmount = 0,
	boneType = "",
	boneAmount = 0,
	milk = 0,
	tamingChance = 0,
	ferocity = 0,
	pvpBitmask = AGGRESSIVE + ATTACKABLE + ENEMY,
	creatureBitmask = KILLER,
	optionsBitmask = AIENABLED,
	diet = HERBIVORE,
	scale = 1.15,

	templates = {"object/mobile/king_terak.iff"},
	lootGroups = {
		{
			groups = {
				{group = "blacksun_rare", chance = 1500000},
				{group = "tfa_paintings", chance = 2000000},
				{group = "dath_schems", chance = 3000000},
				{group = "skill_buffs", chance = 3500000}
			},
			lootChance = 6000000
		}
	},
	weapons = {"nilyahin_tribunal"},
	conversationTemplate = "",
	attacks = merge(tribunal)
}

CreatureTemplates:addCreatureTemplate(nilyahin_tribunal, "nilyahin_tribunal")
