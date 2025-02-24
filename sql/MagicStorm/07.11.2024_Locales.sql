DROP TABLE IF EXISTS `achievement_reward_locale`;
CREATE TABLE IF NOT EXISTS `achievement_reward_locale` (
  `ID` MEDIUMINT(8) UNSIGNED NOT NULL DEFAULT '0',
  `Locale` VARCHAR(4) NOT NULL,
  `Subject` TEXT,
  `Text` TEXT,
  PRIMARY KEY (`ID`, `Locale`)
) ENGINE=MYISAM DEFAULT CHARSET=utf8;


-- locales_achievement_reward
INSERT INTO `achievement_reward_locale` (`ID`, `Locale`, `Subject`, `Text`)
  (SELECT `entry`, "koKR", `subject_loc1`, `text_loc1` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc1) > 0 OR LENGTH(text_loc1) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "frFR", `subject_loc2`, `text_loc2` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc2) > 0 OR LENGTH(text_loc2) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "deDE", `subject_loc3`, `text_loc3` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc3) > 0 OR LENGTH(text_loc3) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "zhCN", `subject_loc4`, `text_loc4` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc4) > 0 OR LENGTH(text_loc4) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "zhTW", `subject_loc5`, `text_loc5` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc5) > 0 OR LENGTH(text_loc5) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "esES", `subject_loc6`, `text_loc6` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc6) > 0 OR LENGTH(text_loc6) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "esMX", `subject_loc7`, `text_loc7` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc7) > 0 OR LENGTH(text_loc7) > 0);

INSERT INTO `achievement_reward_locale` (`ID`, `locale`, `Subject`, `Text`)
  (SELECT `entry`, "ruRU", `subject_loc8`, `text_loc8` FROM `locales_achievement_reward` WHERE LENGTH(subject_loc8) > 0 OR LENGTH(text_loc8) > 0);
  
  DROP TABLE IF EXISTS `locales_achievement_reward`;