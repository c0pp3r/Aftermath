novatrooper_ensign = Creature:new {
	objectName = "@mob/creature_names:stormtrooper_novatrooper_ensign",
	randomNameType = NAME_STORMTROOPER,
	randomNameTag = true,
	socialGroup = "imperial",
	faction = "imperial",
	level = 87,
	chanceHit = 0.85,
	damageMin = 570,
	damageMax = 850,
	baseXp = 8315,
	baseHAM = 13000,
	baseHAMmax = 16000,
	armor = 0,
	resists = {40,35,80,30,0,0,30,45,-1},
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
	creatureBitmask = PACK + KILLER,
	optionsBitmask = AIENABLED,
	diet = HERBIVORE,

	templates = {"object/mobile/dressed_stormtrooper_captain_black_gold.iff"},
	lootGroups = {
		{
			groups = {
				{group = "color_crystals", chance = 700000},
				{group = "junk", chance = 6200000},
				{group = "clothing_attachments", chance = 575000},
				{group = "armor_attachments", chance = 575000},
				{group = "imperial_officer_common", chance = 950000},
				{group = "wearables_scarce", chance = 1000000}
			}
		}
	},
	weapons = {"stormtrooper_weapons"},
	conversationTemplate = "",
	reactionStf = "@npc_reaction/stormtrooper",
	attacks = merge(riflemanmaster,carbineermaster,marksmanmaster,brawlermaster)
}

CreatureTemplates:addCreatureTemplate(novatrooper_ensign, "novatrooper_ensign")
