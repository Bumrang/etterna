#include "global.h"
#include "GameState.h"
#include "Actor.h"
#include "AdjustSync.h"
#include "AnnouncerManager.h"
#include "Bookkeeper.h"
#include "Character.h"
#include "CharacterManager.h"
#include "CommonMetrics.h"
#include "Course.h"
#include "Foreach.h"
#include "Game.h"
#include "GameCommand.h"
#include "GameConstantsAndTypes.h"
#include "GameManager.h"
#include "GamePreferences.h"
#include "HighScore.h"
#include "LightsManager.h"
#include "LuaReference.h"
#include "MessageManager.h"
#include "MemoryCardManager.h"
#include "NoteSkinManager.h"
#include "PlayerState.h"
#include "PrefsManager.h"
#include "Profile.h"
#include "ProfileManager.h"
#include "RageFile.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "song.h"
#include "SongManager.h"
#include "SongUtil.h"
#include "StatsManager.h"
#include "Steps.h"
#include "Style.h"
#include "ThemeManager.h"
#include "UnlockManager.h"
#include "ScreenManager.h"
#include "Screen.h"
#include "WorkoutManager.h"
#include "Workout.h"

#include <ctime>
#include <set>


GameState*	GAMESTATE = NULL;	// global and accessable from anywhere in our program

#define NAME_BLACKLIST_FILE "/Data/NamesBlacklist.txt"

class GameStateMessageHandler: public MessageSubscriber
{
	void HandleMessage( const Message &msg )
	{
		if( msg.GetName() == "RefreshCreditText" )
		{
			RString sJoined;
			FOREACH_HumanPlayer( pn )
			{
				if( sJoined != "" )
					sJoined += ", ";
				sJoined += ssprintf( "P%i", pn+1 );
			}

			if( sJoined == "" )
				sJoined = "none";

			LOG->MapLog( "JOINED", "Players joined: %s", sJoined.c_str() );
		}
	}
};

struct GameStateImpl
{
	GameStateMessageHandler m_Subscriber;
	GameStateImpl()
	{
		m_Subscriber.SubscribeToMessage( "RefreshCreditText" );
	}
};
static GameStateImpl *g_pImpl = NULL;

ThemeMetric<bool> ALLOW_LATE_JOIN("GameState","AllowLateJoin");
ThemeMetric<bool> USE_NAME_BLACKLIST("GameState","UseNameBlacklist");

ThemeMetric<RString> DEFAULT_SORT	("GameState","DefaultSort");
SortOrder GetDefaultSort()
{
	return StringToSortOrder( DEFAULT_SORT );
}
ThemeMetric<RString> DEFAULT_SONG	("GameState","DefaultSong");
Song* GameState::GetDefaultSong() const
{
	SongID sid;
	sid.FromString( DEFAULT_SONG );
	return sid.ToSong();
}

static const ThemeMetric<Grade> GRADE_TIER_FOR_EXTRA_1 ("GameState","GradeTierForExtra1");
static const ThemeMetric<Grade> GRADE_TIER_FOR_EXTRA_2 ("GameState","GradeTierForExtra2");

static ThemeMetric<bool> ARE_STAGE_PLAYER_MODS_FORCED( "GameState","AreStagePlayerModsForced" );
static ThemeMetric<bool> ARE_STAGE_SONG_MODS_FORCED( "GameState","AreStageSongModsForced" );

static Preference<Premium> g_Premium( "Premium", Premium_Off );

GameState::GameState() :
	m_pCurGame(			Message_CurrentGameChanged ),
	m_pCurStyle(			Message_CurrentStyleChanged ),
	m_PlayMode(			Message_PlayModeChanged ),
	m_sPreferredSongGroup(		Message_PreferredSongGroupChanged ),
	m_sPreferredCourseGroup(	Message_PreferredCourseGroupChanged ),
	m_PreferredStepsType(		Message_PreferredStepsTypeChanged ),
	m_PreferredDifficulty(		Message_PreferredDifficultyP1Changed ),
	m_PreferredCourseDifficulty(	Message_PreferredCourseDifficultyP1Changed ),
	m_SortOrder(			Message_SortOrderChanged ),
	m_pCurSong(			Message_CurrentSongChanged ),
	m_pCurSteps(			Message_CurrentStepsP1Changed ),
	m_pCurCourse(			Message_CurrentCourseChanged ),
	m_pCurTrail(			Message_CurrentTrailP1Changed ),
	m_bGameplayLeadIn(		Message_GameplayLeadInChanged ),
	m_bDidModeChangeNoteSkin(	false ),
	m_stEdit(			Message_EditStepsTypeChanged ),
	m_cdEdit(			Message_EditCourseDifficultyChanged ),
	m_pEditSourceSteps(		Message_EditSourceStepsChanged ),
	m_stEditSource(			Message_EditSourceStepsTypeChanged ),
	m_iEditCourseEntryIndex(	Message_EditCourseEntryIndexChanged ),
	m_sEditLocalProfileID(		Message_EditLocalProfileIDChanged )
{
	g_pImpl = new GameStateImpl;

	SetCurrentStyle( NULL );

	m_pCurGame.Set( NULL );
	m_iCoins = 0;
	m_timeGameStarted.SetZero();
	m_bDemonstrationOrJukebox = false;

	m_iNumTimesThroughAttract = -1;	// initial screen will bump this up to 0
	m_iStageSeed = m_iGameSeed = 0;

	m_PlayMode.Set( PlayMode_Invalid ); // used by IsPlayerEnabled before the first screen
	FOREACH_PlayerNumber( p )
		m_bSideIsJoined[p] = false; // used by GetNumSidesJoined before the first screen

	FOREACH_PlayerNumber( p )
	{
		m_pPlayerState[p] = new PlayerState;
		m_pPlayerState[p]->m_PlayerNumber = p;
	}
	FOREACH_MultiPlayer( p )
	{
		m_pMultiPlayerState[p] = new PlayerState;
		m_pMultiPlayerState[p]->m_PlayerNumber = PLAYER_1;
		m_pMultiPlayerState[p]->m_mp = p;
	}

	m_Environment = new LuaTable;

	/* Don't reset yet; let the first screen do it, so we can
	 * use PREFSMAN and THEME. */
//	Reset();

	// Register with Lua.
	{
		Lua *L = LUA->Get();
		lua_pushstring( L, "GAMESTATE" );
		this->PushSelf( L );
		lua_settable( L, LUA_GLOBALSINDEX );
		LUA->Release( L );
	}
}

GameState::~GameState()
{
	// Unregister with Lua.
	LUA->UnsetGlobal( "GAMESTATE" );

	FOREACH_PlayerNumber( p )
		SAFE_DELETE( m_pPlayerState[p] );
	FOREACH_MultiPlayer( p )
		SAFE_DELETE( m_pMultiPlayerState[p] );

	SAFE_DELETE( m_Environment );
	SAFE_DELETE( g_pImpl );
}

void GameState::ApplyGameCommand( const RString &sCommand, PlayerNumber pn )
{
	GameCommand m;
	m.Load( 0, ParseCommands(sCommand) );

	RString sWhy;
	if( !m.IsPlayable(&sWhy) )
		RageException::Throw( "Can't apply mode \"%s\": %s", sCommand.c_str(), sWhy.c_str() );

	if( pn == PLAYER_INVALID )
		m.ApplyToAllPlayers();
	else
		m.Apply( pn );
}

void GameState::ApplyCmdline()
{
	/* We need to join players before we can set the style. */
	RString sPlayer;
	for( int i = 0; GetCommandlineArgument( "player", &sPlayer, i ); ++i )
	{
		int pn = atoi( sPlayer )-1;
		if( !IsAnInt( sPlayer ) || pn < 0 || pn >= NUM_PLAYERS )
			RageException::Throw( "Invalid argument \"--player=%s\".", sPlayer.c_str() );

		JoinPlayer( (PlayerNumber) pn );
	}

	RString sMode;
	for( int i = 0; GetCommandlineArgument( "mode", &sMode, i ); ++i )
	{
		ApplyGameCommand( sMode );
	}
}

void GameState::ResetPlayer( PlayerNumber pn )
{
	m_PreferredStepsType.Set( StepsType_Invalid );
	m_PreferredDifficulty[pn].Set( Difficulty_Invalid );
	m_PreferredCourseDifficulty[pn].Set( Difficulty_Medium );
	m_iPlayerStageTokens[pn] = 0;
	m_iAwardedExtraStages[pn] = 0;
	m_pCurSteps[pn].Set( NULL );
	m_pCurTrail[pn].Set( NULL );
	m_pPlayerState[pn]->Reset();
	PROFILEMAN->UnloadProfile( pn );

	PlayerOptions po;
	GetDefaultPlayerOptions( po );
	m_pPlayerState[pn]->m_PlayerOptions.Assign( ModsLevel_Preferred, po );
}

void GameState::Reset()
{
	m_MasterPlayerNumber = PLAYER_INVALID; // must initialize for UnjoinPlayer

	FOREACH_PlayerNumber( pn )
		UnjoinPlayer( pn );

	ASSERT( THEME );

	m_timeGameStarted.SetZero();
	SetCurrentStyle( NULL );
	FOREACH_MultiPlayer( p )
		m_MultiPlayerStatus[p] = MultiPlayerStatus_NotJoined;
	FOREACH_PlayerNumber( pn )
		MEMCARDMAN->UnlockCard( pn );
//	m_iCoins = 0;	// don't reset coin count!
	m_bMultiplayer = false;
	m_iNumMultiplayerNoteFields = 1;
	*m_Environment = LuaTable();
	m_sPreferredSongGroup.Set( GROUP_ALL );
	m_sPreferredCourseGroup.Set( GROUP_ALL );
	m_bChangedFailTypeOnScreenSongOptions = false;
	m_SortOrder.Set( SortOrder_Invalid );
	m_PreferredSortOrder = GetDefaultSort();
	m_PlayMode.Set( PlayMode_Invalid );
	m_EditMode = EditMode_Invalid;
	m_bDemonstrationOrJukebox = false;
	m_bJukeboxUsesModifiers = false;
	m_iCurrentStageIndex = 0;

	m_bGameplayLeadIn.Set( false );
	m_iNumStagesOfThisSong = 0;
	m_bLoadingNextSong = false;

	NOTESKIN->RefreshNoteSkinData( m_pCurGame );

	m_iGameSeed = rand();
	m_iStageSeed = rand();

	m_pCurSong.Set( GetDefaultSong() );
	m_pPreferredSong = NULL;
	m_pCurCourse.Set( NULL );
	m_pPreferredCourse = NULL;

	FOREACH_MultiPlayer( p )
		m_pMultiPlayerState[p]->Reset();

	m_SongOptions.Init();

	ResetMusicStatistics();
	ResetStageStatistics();
	AdjustSync::ResetOriginalSyncData();

	SONGMAN->UpdatePopular();
	SONGMAN->UpdateShuffled();

	/* We may have cached trails from before everything was loaded (eg. from before
	 * SongManager::UpdatePopular could be called).  Erase the cache. */
	SONGMAN->RegenerateNonFixedCourses();

	STATSMAN->Reset();

	FOREACH_PlayerNumber(p)
	{
		if( PREFSMAN->m_ShowDancingCharacters == SDC_Random )
			m_pCurCharacters[p] = CHARMAN->GetRandomCharacter();
		else
			m_pCurCharacters[p] = CHARMAN->GetDefaultCharacter();
		ASSERT( m_pCurCharacters[p] );
	}

	m_bTemporaryEventMode = false;

	LIGHTSMAN->SetLightsMode( LIGHTSMODE_ATTRACT );
	
	m_stEdit.Set( StepsType_Invalid );
	m_pEditSourceSteps.Set( NULL );
	m_stEditSource.Set( StepsType_Invalid );
	m_iEditCourseEntryIndex.Set( -1 );
	m_sEditLocalProfileID.Set( "" );
	
	m_bBackedOutOfFinalStage = false;
	m_bEarnedExtraStage = false;
	ApplyCmdline();
}

void GameState::JoinPlayer( PlayerNumber pn )
{
	/* If joint premium and we're not taking away a credit for the 2nd join,
	 * give the new player the same number of stage tokens that the old player
	 * has. */
	if( GetCoinMode() == CoinMode_Pay && GetPremium() == Premium_2PlayersFor1Credit && GetNumSidesJoined() == 1 )
		m_iPlayerStageTokens[pn] = m_iPlayerStageTokens[m_MasterPlayerNumber];
	else
		m_iPlayerStageTokens[pn] = PREFSMAN->m_iSongsPerPlay;

	m_bSideIsJoined[pn] = true;

	if( m_MasterPlayerNumber == PLAYER_INVALID )
		m_MasterPlayerNumber = pn;

	// if first player to join, set start time
	if( GetNumSidesJoined() == 1 )
		BeginGame();

	/* Count each player join as a play. */
	{
		Profile* pMachineProfile = PROFILEMAN->GetMachineProfile();
		pMachineProfile->m_iTotalPlays++;
	}

	// Set the current style to something appropriate for the new number of joined players.
	if( ALLOW_LATE_JOIN  &&  m_pCurStyle != NULL )
	{
		const Style *pStyle = GAMEMAN->GetFirstCompatibleStyle( m_pCurGame, GetNumSidesJoined(), m_pCurStyle->m_StepsType );
		m_pCurStyle.Set( pStyle );
	}

	Message msg( MessageIDToString(Message_PlayerJoined) );
	msg.SetParam( "Player", pn );
	MESSAGEMAN->Broadcast( msg );
}

void GameState::UnjoinPlayer( PlayerNumber pn )
{
	m_bSideIsJoined[pn] = false;
	m_iPlayerStageTokens[pn] = 0;

	ResetPlayer( pn );

	if( m_MasterPlayerNumber == pn )
		m_MasterPlayerNumber = GetFirstHumanPlayer();

	/* Unjoin STATSMAN first, so steps used by this player are released
	 * and can be released by PROFILEMAN. */
	STATSMAN->UnjoinPlayer( pn );
	PROFILEMAN->UnloadProfile( pn );

	Message msg( MessageIDToString(Message_PlayerUnjoined) );
	msg.SetParam( "Player", pn );
	MESSAGEMAN->Broadcast( msg );

	/* If there are no players left, reset some non-player-specific stuff, too. */
	if( m_MasterPlayerNumber == PLAYER_INVALID )
	{
		SongOptions so;
		GetDefaultSongOptions( so );
		m_SongOptions.Assign( ModsLevel_Preferred, so );
		m_bDidModeChangeNoteSkin = false;
	}
}

/* Handle an input that can join a player.  Return true if the player joined. */
bool GameState::JoinInput( PlayerNumber pn )
{
	if( !PlayersCanJoin() )
		return false;

	/* If this side is already in, don't re-join. */
	if( m_bSideIsJoined[pn] )
		return false;

	/* subtract coins */
	int iCoinsNeededToJoin = GetCoinsNeededToJoin();

	if( m_iCoins < iCoinsNeededToJoin )
		return false;	// not enough coins

	m_iCoins -= iCoinsNeededToJoin;

	JoinPlayer( pn );

	return true;
}

int GameState::GetCoinsNeededToJoin() const
{
	int iCoinsToCharge = 0;

	if( GetCoinMode() == CoinMode_Pay )
		iCoinsToCharge = PREFSMAN->m_iCoinsPerCredit;

	// If joint premium don't take away a credit for the 2nd join.
	if( GetPremium() == Premium_2PlayersFor1Credit  &&  
		GetNumSidesJoined() == 1 )
		iCoinsToCharge = 0;

	return iCoinsToCharge;
}

/*
 * Game flow:
 *
 * BeginGame() - the first player has joined; the game is starting.
 *
 * PlayersFinalized() - player memory cards are loaded; later joins won't have memory cards this stage
 *
 * BeginStage() - gameplay is beginning
 *
 * optional: CancelStage() - gameplay aborted (Back pressed), undo BeginStage and back up
 *
 * CommitStageStats() - gameplay is finished
 *   Saves STATSMAN->m_CurStageStats to the profiles, so profile information
 *   is up-to-date for Evaluation.
 *
 * FinishStage() - gameplay and evaluation is finished
 *   Clears data which was stored by CommitStageStats.
 */
void GameState::BeginGame()
{
	m_timeGameStarted.Touch();

	m_vpsNamesThatWereFilled.clear();

	// Play attract on the ending screen, then on the ranking screen
	// even if attract sounds are set to off.
	m_iNumTimesThroughAttract = -1;

	FOREACH_PlayerNumber( pn )
		MEMCARDMAN->UnlockCard( pn );
}

void GameState::LoadProfiles( bool bLoadEdits )
{
	/* Unlock any cards that we might want to load. */
	FOREACH_HumanPlayer( pn )
		if( !PROFILEMAN->IsPersistentProfile(pn) )
			MEMCARDMAN->UnlockCard( pn );

	MEMCARDMAN->WaitForCheckingToComplete();

	FOREACH_HumanPlayer( pn )
	{
		// If a profile is already loaded, this was already called.
		if( PROFILEMAN->IsPersistentProfile(pn) )
			continue;

		MEMCARDMAN->MountCard( pn );
		bool bSuccess = PROFILEMAN->LoadFirstAvailableProfile( pn, bLoadEdits );	// load full profile
		MEMCARDMAN->UnmountCard( pn );

		if( !bSuccess )
			continue;

		/* Lock the card on successful load, so we won't allow it to be changed. */
		MEMCARDMAN->LockCard( pn );

		LoadCurrentSettingsFromProfile( pn );

		Profile* pPlayerProfile = PROFILEMAN->GetProfile( pn );
		if( pPlayerProfile )
			pPlayerProfile->m_iTotalPlays++;
	}
}

void GameState::SaveProfiles()
{
	FOREACH_HumanPlayer( pn )
		SaveProfile( pn );
}

void GameState::SaveProfile( PlayerNumber pn )
{
	if( !PROFILEMAN->IsPersistentProfile(pn) )
		return;

	bool bWasMemoryCard = PROFILEMAN->ProfileWasLoadedFromMemoryCard(pn);
	if( bWasMemoryCard )
		MEMCARDMAN->MountCard( pn );
	PROFILEMAN->SaveProfile( pn );
	if( bWasMemoryCard )
		MEMCARDMAN->UnmountCard( pn );
}

bool GameState::HaveProfileToLoad()
{
	FOREACH_HumanPlayer( pn )
	{
		/* We won't load this profile if it's already loaded. */
		if( PROFILEMAN->IsPersistentProfile(pn) )
			continue;

		/* If a memory card is inserted, we'l try to load it. */
		if( MEMCARDMAN->CardInserted(pn) )
			return true;
		if( !PROFILEMAN->m_sDefaultLocalProfileID[pn].Get().empty() )
			return true;
	}

	return false;
}

bool GameState::HaveProfileToSave()
{
	FOREACH_HumanPlayer( pn )
		if( PROFILEMAN->IsPersistentProfile(pn) )
			return true;
	return false;
}

void GameState::SaveLocalData()
{
	BOOKKEEPER->WriteToDisk();
	PROFILEMAN->SaveMachineProfile();
}

int GameState::GetNumStagesMultiplierForSong( const Song* pSong )
{
	int iNumStages = 1;

	ASSERT( pSong );
	if( pSong->IsMarathon() )
		iNumStages *= 3;
	if( pSong->IsLong() )
		iNumStages *= 2;

	return iNumStages;
}

int GameState::GetNumStagesForSongAndStyleType( const Song* pSong, StyleType st )
{
	int iNumStages = GetNumStagesMultiplierForSong( pSong );

	// One player, two-sides styles cost extra 
	switch( st )
	{
	DEFAULT_FAIL( st );
	case StyleType_OnePlayerTwoSides:
		if( g_Premium == Premium_Off )
			iNumStages *= 2;
		break;
	case StyleType_TwoPlayersTwoSides:
	case StyleType_OnePlayerOneSide:
	case StyleType_TwoPlayersSharedSides:
		break;
	}

	return iNumStages;
}

int GameState::GetNumStagesForCurrentSongAndStepsOrCourse() const
{
	int iNumStagesOfThisSong = 1;
	if( m_pCurSong )
	{
		const Style *pStyle = m_pCurStyle;
		if( pStyle == NULL )
		{
			const Steps *pSteps = m_pCurSteps[m_MasterPlayerNumber];
			if( pSteps )
			{
				/* If a style isn't set, use the style of the selected steps. */
				StepsType st = pSteps->m_StepsType;
				pStyle = GAMEMAN->GetFirstCompatibleStyle( m_pCurGame, GetNumSidesJoined(), st );
			}
			else
			{
				/* If steps aren't set either, pick any style for the number of
				 * joined players, or one player if no players are joined. */
				vector<const Style*> vpStyles;
				int iJoined = max( GetNumSidesJoined(), 1 );
				GAMEMAN->GetCompatibleStyles( m_pCurGame, iJoined, vpStyles );
				ASSERT( !vpStyles.empty() );
				pStyle = vpStyles[0];
			}
		}
		iNumStagesOfThisSong = GameState::GetNumStagesForSongAndStyleType( m_pCurSong, pStyle->m_StyleType );
	}
	else if( m_pCurCourse )
		iNumStagesOfThisSong = PREFSMAN->m_iSongsPerPlay;
	else
		return -1;

	iNumStagesOfThisSong = max( iNumStagesOfThisSong, 1 );

	return iNumStagesOfThisSong;
}

/* Called by ScreenGameplay.  Set the length of the current song. */
void GameState::BeginStage()
{
	if( m_bDemonstrationOrJukebox )
		return;

	/* This should only be called once per stage. */
	if( m_iNumStagesOfThisSong != 0 )
		LOG->Warn( "XXX: m_iNumStagesOfThisSong == %i?", m_iNumStagesOfThisSong );

	ResetStageStatistics();
	AdjustSync::ResetOriginalSyncData();

	if( !ARE_STAGE_PLAYER_MODS_FORCED )
	{
		FOREACH_PlayerNumber( p )
			m_pPlayerState[p]->m_PlayerOptions.Assign( ModsLevel_Stage, m_pPlayerState[p]->m_PlayerOptions.GetPreferred() );
	}
	if( !ARE_STAGE_SONG_MODS_FORCED )
		m_SongOptions.Assign( ModsLevel_Stage, m_SongOptions.GetPreferred() );

	STATSMAN->m_CurStageStats.m_fMusicRate = m_SongOptions.GetSong().m_fMusicRate;
	m_iNumStagesOfThisSong = GetNumStagesForCurrentSongAndStepsOrCourse();
	ASSERT( m_iNumStagesOfThisSong != -1 );
	FOREACH_EnabledPlayer( p )
		m_iPlayerStageTokens[p] -= m_iNumStagesOfThisSong;

	FOREACH_HumanPlayer( pn )
		if( CurrentOptionsDisqualifyPlayer(pn) )
			STATSMAN->m_CurStageStats.m_player[pn].m_bDisqualified = true;
	m_bEarnedExtraStage = false;
}

void GameState::CancelStage()
{
	FOREACH_EnabledPlayer( p )
		m_iPlayerStageTokens[p] += m_iNumStagesOfThisSong;
	m_iNumStagesOfThisSong = 0;
	ResetStageStatistics();
}

void GameState::CommitStageStats()
{
	if( m_bDemonstrationOrJukebox )
		return;
	if( m_bMultiplayer )
		return;

	STATSMAN->CommitStatsToProfiles( &STATSMAN->m_CurStageStats );

	// Update TotalPlaySeconds.
	int iPlaySeconds = max( 0, (int) m_timeGameStarted.GetDeltaTime() );

	Profile* pMachineProfile = PROFILEMAN->GetMachineProfile();
	pMachineProfile->m_iTotalPlaySeconds += iPlaySeconds;

	FOREACH_HumanPlayer( p )
	{
		Profile* pPlayerProfile = PROFILEMAN->GetProfile( p );
		if( pPlayerProfile )
			pPlayerProfile->m_iTotalPlaySeconds += iPlaySeconds;
	}
}

/* Called by ScreenSelectMusic (etc).  Increment the stage counter if we just played a
 * song.  Might be called more than once. */
void GameState::FinishStage()
{
	// Increment the stage counter.
	const int iOldStageIndex = m_iCurrentStageIndex;

	++m_iCurrentStageIndex;

	m_iNumStagesOfThisSong = 0;

	if( HasEarnedExtraStageInternal() )
	{
		LOG->Trace( "awarded extra stage" );
		FOREACH_HumanPlayer( p )
		{
			if( m_iAwardedExtraStages[p] < 2 )
			{
				++m_iAwardedExtraStages[p];
				++m_iPlayerStageTokens[p];
				m_bEarnedExtraStage = true;
			}
		}
	}

	if( m_bDemonstrationOrJukebox )
		return;

	if( IsEventMode() )
	{
		const int iSaveProfileEvery = 3;
		if( iOldStageIndex/iSaveProfileEvery < m_iCurrentStageIndex/iSaveProfileEvery )
		{
			LOG->Trace( "Played %i stages; saving profiles ...", iSaveProfileEvery );
			PROFILEMAN->SaveMachineProfile();
			this->SaveProfiles();
		}
	}
}

void GameState::LoadCurrentSettingsFromProfile( PlayerNumber pn )
{
	if( !PROFILEMAN->IsPersistentProfile(pn) )
		return;

	const Profile *pProfile = PROFILEMAN->GetProfile(pn);

	// apply saved default modifiers if any
	RString sModifiers;
	if( pProfile->GetDefaultModifiers( m_pCurGame, sModifiers ) )
	{
		/* We don't save negative preferences (eg. "no reverse").  If the theme
		 * sets a default of "reverse", and the player turns it off, we should
		 * set it off.  However, don't reset modifiers that aren't saved by the
		 * profile, so we don't ignore unsaved modifiers when a profile is in use. */
		PO_GROUP_CALL( m_pPlayerState[pn]->m_PlayerOptions, ModsLevel_Preferred, ResetSavedPrefs );
		ApplyPreferredModifiers( pn, sModifiers );
	}
	// Only set the sort order if it wasn't already set by a GameCommand (or by an earlier profile)
	if( m_PreferredSortOrder == SortOrder_Invalid  &&  pProfile->m_SortOrder != SortOrder_Invalid )
		m_PreferredSortOrder = pProfile->m_SortOrder;
	if( pProfile->m_LastDifficulty != Difficulty_Invalid )
		m_PreferredDifficulty[pn].Set( pProfile->m_LastDifficulty );
	if( pProfile->m_LastCourseDifficulty != Difficulty_Invalid )
		m_PreferredCourseDifficulty[pn].Set( pProfile->m_LastCourseDifficulty );
	// Only set the PreferredStepsType if it wasn't already set by a GameCommand (or by an earlier profile)
	if( m_PreferredStepsType == StepsType_Invalid  &&  pProfile->m_LastStepsType != StepsType_Invalid )
		m_PreferredStepsType.Set( pProfile->m_LastStepsType );
	if( m_pPreferredSong == NULL )
		m_pPreferredSong = pProfile->m_lastSong.ToSong();
	if( m_pPreferredCourse == NULL )
		m_pPreferredCourse = pProfile->m_lastCourse.ToCourse();
}

void GameState::SaveCurrentSettingsToProfile( PlayerNumber pn )
{
	if( !PROFILEMAN->IsPersistentProfile(pn) )
		return;
	if( m_bDemonstrationOrJukebox )
		return;

	Profile* pProfile = PROFILEMAN->GetProfile(pn);

	pProfile->SetDefaultModifiers( m_pCurGame, m_pPlayerState[pn]->m_PlayerOptions.GetPreferred().GetSavedPrefsString() );
	if( IsSongSort(m_PreferredSortOrder) )
		pProfile->m_SortOrder = m_PreferredSortOrder;
	if( m_PreferredDifficulty[pn] != Difficulty_Invalid )
		pProfile->m_LastDifficulty = m_PreferredDifficulty[pn];
	if( m_PreferredCourseDifficulty[pn] != Difficulty_Invalid )
		pProfile->m_LastCourseDifficulty = m_PreferredCourseDifficulty[pn];
	if( m_PreferredStepsType != StepsType_Invalid )
		pProfile->m_LastStepsType = m_PreferredStepsType;
	if( m_pPreferredSong )
		pProfile->m_lastSong.FromSong( m_pPreferredSong );
	if( m_pPreferredCourse )
		pProfile->m_lastCourse.FromCourse( m_pPreferredCourse );
}

void GameState::Update( float fDelta )
{
	m_SongOptions.Update( fDelta );

	FOREACH_PlayerNumber( p )
	{
		m_pPlayerState[p]->Update( fDelta );

		if( !m_bGoalComplete[p] && IsGoalComplete(p) )
		{
			m_bGoalComplete[p] = true;
			MESSAGEMAN->Broadcast( (MessageID)(Message_GoalCompleteP1+p) );
		}
	}

	if( WORKOUTMAN->m_pCurWorkout != NULL  &&  !m_bWorkoutGoalComplete )
	{
		const StageStats &ssCurrent = STATSMAN->m_CurStageStats;
		bool bGoalComplete = ssCurrent.m_fGameplaySeconds > WORKOUTMAN->m_pCurWorkout->m_iMinutes*60;
		if( bGoalComplete )
		{
			MESSAGEMAN->Broadcast( "WorkoutGoalComplete" );
			m_bWorkoutGoalComplete = true;
		}
	}
}

void GameState::SetCurGame( const Game *pGame )
{
	m_pCurGame.Set( pGame );
	RString sGame = pGame ? RString(pGame->m_szName) : RString();
	PREFSMAN->SetCurrentGame( sGame );
}

const float GameState::MUSIC_SECONDS_INVALID = -5000.0f;

void GameState::ResetMusicStatistics()
{	
	m_fMusicSeconds = 0; // MUSIC_SECONDS_INVALID;
	m_fSongBeat = 0;
	m_fSongBeatNoOffset = 0;
	m_fCurBPS = 10;
	m_bFreeze = false;
	m_fMusicSecondsVisible = 0;
	m_fSongBeatVisible = 0;
	Actor::SetBGMTime( 0, 0, 0, 0 );


	FOREACH_PlayerNumber( p )
		m_pPlayerState[p]->ClearHopoState();
}

void GameState::ResetStageStatistics()
{
	STATSMAN->m_CurStageStats = StageStats();

	RemoveAllActiveAttacks();
	FOREACH_PlayerNumber( p )
		m_pPlayerState[p]->RemoveAllInventory();
	m_fOpponentHealthPercent = 1;
	m_fHasteRate = 0;
	m_fLastHasteUpdateMusicSeconds = 0;
	m_fAccumulatedHasteSeconds = 0;
	m_fTugLifePercentP1 = 0.5f;
	FOREACH_PlayerNumber( p )
	{
		m_pPlayerState[p]->m_fSuperMeter = 0;
		m_pPlayerState[p]->m_HealthState = HealthState_Alive;

		m_pPlayerState[p]->m_iLastPositiveSumOfAttackLevels = 0;
		m_pPlayerState[p]->m_fSecondsUntilAttacksPhasedOut = 0;	// PlayerAI not affected

		m_bGoalComplete[p] = false;
	}
	m_bWorkoutGoalComplete = false;


	FOREACH_PlayerNumber( p )
	{
		m_vLastPerDifficultyAwards[p].clear();
		m_vLastPeakComboAwards[p].clear();
	}

	// Reset the round seed.  Do this here and not in FinishStage so that players
	// get new shuffle patterns if they Back out of gameplay and play again.
	m_iStageSeed = rand();
}

static Preference<float> g_fVisualDelaySeconds( "VisualDelaySeconds", 0.0f );
void GameState::UpdateSongPosition( float fPositionSeconds, const TimingData &timing, const RageTimer &timestamp )
{
	if( !timestamp.IsZero() )
		m_LastBeatUpdate = timestamp;
	else
		m_LastBeatUpdate.Touch();
	timing.GetBeatAndBPSFromElapsedTime( fPositionSeconds, m_fSongBeat, m_fCurBPS, m_bFreeze );
	ASSERT_M( m_fSongBeat > -2000, ssprintf("%f %f", m_fSongBeat, fPositionSeconds) );

	m_fMusicSeconds = fPositionSeconds;
	m_fLightSongBeat = timing.GetBeatFromElapsedTime( fPositionSeconds + g_fLightsAheadSeconds );

	m_fSongBeatNoOffset = timing.GetBeatFromElapsedTimeNoOffset( fPositionSeconds );

	m_fMusicSecondsVisible = fPositionSeconds - g_fVisualDelaySeconds.Get();
	float fThrowAway;
	bool bThrowAway;
	timing.GetBeatAndBPSFromElapsedTime( m_fMusicSecondsVisible, m_fSongBeatVisible, fThrowAway, bThrowAway );

	Actor::SetBGMTime( m_fMusicSecondsVisible, m_fSongBeatVisible, fPositionSeconds, m_fSongBeatNoOffset );
//	LOG->Trace( "m_fMusicSeconds = %f, m_fSongBeat = %f, m_fCurBPS = %f, m_bFreeze = %f", m_fMusicSeconds, m_fSongBeat, m_fCurBPS, m_bFreeze );
}

float GameState::GetSongPercent( float beat ) const
{
	/* 0 = first step; 1 = last step */
	return (beat - m_pCurSong->m_fFirstBeat) / m_pCurSong->m_fLastBeat;
}

int GameState::GetNumStagesLeft( PlayerNumber pn ) const
{
	return m_iPlayerStageTokens[pn];
}

int GameState::GetSmallestNumStagesLeftForAnyHumanPlayer() const
{
	if( IsEventMode() )
		return 999;
	int iSmallest = INT_MAX;
	FOREACH_HumanPlayer( p )
		iSmallest = min( iSmallest, m_iPlayerStageTokens[p] );
	return iSmallest;
}

bool GameState::IsAnExtraStage() const
{
	if( m_MasterPlayerNumber == PlayerNumber_Invalid )
		return false;
	return !IsEventMode() && !IsCourseMode() && m_iAwardedExtraStages[m_MasterPlayerNumber] > 0;
}

bool GameState::IsExtraStage() const
{
	if( m_MasterPlayerNumber == PlayerNumber_Invalid )
		return false;
	return !IsEventMode() && !IsCourseMode() && m_iAwardedExtraStages[m_MasterPlayerNumber] == 1;
}

bool GameState::IsExtraStage2() const
{
	if( m_MasterPlayerNumber == PlayerNumber_Invalid )
		return false;
	return !IsEventMode() && !IsCourseMode() && m_iAwardedExtraStages[m_MasterPlayerNumber] == 2;
}

Stage GameState::GetCurrentStage() const
{
	if( m_bDemonstrationOrJukebox )			return STAGE_DEMO;
	// "event" has precedence
	else if( IsEventMode() )			return STAGE_EVENT;
	else if( m_PlayMode == PLAY_MODE_ONI )		return STAGE_ONI;
	else if( m_PlayMode == PLAY_MODE_NONSTOP )	return STAGE_NONSTOP;
	else if( m_PlayMode == PLAY_MODE_ENDLESS )	return STAGE_ENDLESS;
	else if( IsExtraStage() )			return STAGE_EXTRA1;
	else if( IsExtraStage2() )			return STAGE_EXTRA2;
	else						return STAGE_NORMAL;
}

int GameState::GetCourseSongIndex() const
{
	/* iSongsPlayed includes the current song, so it's 1-based; subtract one. */
	if( GAMESTATE->m_bMultiplayer )
	{
		FOREACH_EnabledMultiPlayer(mp)
			return STATSMAN->m_CurStageStats.m_multiPlayer[mp].m_iSongsPlayed-1;
		FAIL_M("At least one MultiPlayer must be joined.");
	}
	else
	{
		return STATSMAN->m_CurStageStats.m_player[m_MasterPlayerNumber].m_iSongsPlayed-1;
	}
}

/* Hack: when we're loading a new course song, we want to display the new song number, even
 * though we haven't started that song yet. */
int GameState::GetLoadingCourseSongIndex() const
{
	int iIndex = GetCourseSongIndex();
	if( m_bLoadingNextSong )
		++iIndex;
	return iIndex;
}

static LocalizedString PLAYER1	("GameState","Player 1");
static LocalizedString PLAYER2	("GameState","Player 2");
static LocalizedString CPU		("GameState","CPU");
RString GameState::GetPlayerDisplayName( PlayerNumber pn ) const
{
	ASSERT( IsPlayerEnabled(pn) );
	const LocalizedString *pDefaultNames[] = { &PLAYER1, &PLAYER2 };
	if( IsHumanPlayer(pn) )
	{
		if( !PROFILEMAN->GetPlayerName(pn).empty() )
			return PROFILEMAN->GetPlayerName(pn);
		else
			return pDefaultNames[pn]->GetValue();
	}
	else
	{
		return CPU.GetValue();
	}
}

bool GameState::PlayersCanJoin() const
{
	bool b = GetNumSidesJoined() == 0 || GetCurrentStyle() == NULL;	// selecting a style finalizes the players
	if( ALLOW_LATE_JOIN.IsLoaded()  &&  ALLOW_LATE_JOIN )
	{
		Screen *pScreen = SCREENMAN->GetTopScreen();
		if( pScreen )
			b |= pScreen->AllowLateJoin();
	}
	return b;
}

int GameState::GetNumSidesJoined() const
{ 
	int iNumSidesJoined = 0;
	FOREACH_PlayerNumber( p )
		if( m_bSideIsJoined[p] )
			iNumSidesJoined++;	// left side, and right side
	return iNumSidesJoined;
}

const Game* GameState::GetCurrentGame()
{
	ASSERT( m_pCurGame != NULL );	// the game must be set before calling this
	return m_pCurGame;
}

const Style* GameState::GetCurrentStyle() const
{
	return m_pCurStyle;
}

void GameState::SetCurrentStyle( const Style *pStyle )
{
	m_pCurStyle.Set( pStyle );
	if( INPUTMAPPER )
	{
		if( GetCurrentStyle() && GetCurrentStyle()->m_StyleType == StyleType_OnePlayerTwoSides )
			INPUTMAPPER->SetJoinControllers( m_MasterPlayerNumber );
		else
			INPUTMAPPER->SetJoinControllers( PLAYER_INVALID );
	}
}

bool GameState::IsPlayerEnabled( PlayerNumber pn ) const
{
	// In rave, all players are present.  Non-human players are CPU controlled.
	switch( m_PlayMode )
	{
	case PLAY_MODE_BATTLE:
	case PLAY_MODE_RAVE:
		return true;
	}

	return IsHumanPlayer( pn );
}

bool GameState::IsMultiPlayerEnabled( MultiPlayer mp ) const
{
	return m_MultiPlayerStatus[ mp ] == MultiPlayerStatus_Joined;
}

bool GameState::IsPlayerEnabled( const PlayerState* pPlayerState ) const
{
	if( pPlayerState->m_mp != MultiPlayer_Invalid )
		return IsMultiPlayerEnabled( pPlayerState->m_mp );
	if( pPlayerState->m_PlayerNumber != PLAYER_INVALID )
		return IsPlayerEnabled( pPlayerState->m_PlayerNumber );
	return false;
}

int	GameState::GetNumPlayersEnabled() const
{
	int count = 0;
	FOREACH_EnabledPlayer( pn )
		count++;
	return count;
}

bool GameState::IsHumanPlayer( PlayerNumber pn ) const
{
	if( pn == PLAYER_INVALID )
		return false;

	if( GetCurrentStyle() == NULL )	// no style chosen
	{
		if( PlayersCanJoin() )	
			return m_bSideIsJoined[pn];	// only allow input from sides that have already joined
		else
			return true;	// if we can't join, then we're on a screen like MusicScroll or GameOver
	}

	switch( GetCurrentStyle()->m_StyleType )
	{
	case StyleType_TwoPlayersTwoSides:
	case StyleType_TwoPlayersSharedSides:
		return true;
	case StyleType_OnePlayerOneSide:
	case StyleType_OnePlayerTwoSides:
		return pn == m_MasterPlayerNumber;
	default:
		ASSERT(0);		// invalid style type
		return false;
	}
}

int GameState::GetNumHumanPlayers() const
{
	int count = 0;
	FOREACH_HumanPlayer( pn )
		count++;
	return count;
}

PlayerNumber GameState::GetFirstHumanPlayer() const
{
	FOREACH_HumanPlayer( pn )
		return pn;
	return PLAYER_INVALID;
}

PlayerNumber GameState::GetFirstDisabledPlayer() const
{
	FOREACH_PlayerNumber( pn )
		if( !IsPlayerEnabled(pn) )
			return pn;
	return PLAYER_INVALID;
}

bool GameState::IsCpuPlayer( PlayerNumber pn ) const
{
	return IsPlayerEnabled(pn) && !IsHumanPlayer(pn);
}

bool GameState::AnyPlayersAreCpu() const
{ 
	FOREACH_CpuPlayer( pn )
		return true;
	return false;
}


bool GameState::IsCourseMode() const
{
	switch(m_PlayMode)
	{
	case PLAY_MODE_ONI:
	case PLAY_MODE_NONSTOP:
	case PLAY_MODE_ENDLESS:
		return true;
	default:
		return false;
	}
}

bool GameState::IsBattleMode() const
{
	switch( m_PlayMode )
	{
	case PLAY_MODE_BATTLE:
		return true;
	default:
		return false;
	}
}	

bool GameState::HasEarnedExtraStageInternal() const
{
	if( IsEventMode() )
		return false;

	if( !PREFSMAN->m_bAllowExtraStage )
		return false;

	if( m_PlayMode != PLAY_MODE_REGULAR )
		return false;
	
	if( m_bBackedOutOfFinalStage )
		return false;
	
	if( GetSmallestNumStagesLeftForAnyHumanPlayer() > 0 )
		return false;

	if( m_iAwardedExtraStages[m_MasterPlayerNumber] >= 2 )
		return false;
	
	FOREACH_EnabledPlayer( pn )
	{
		if( m_pCurSteps[pn]->GetDifficulty() != Difficulty_Hard && 
		    m_pCurSteps[pn]->GetDifficulty() != Difficulty_Challenge )
			continue; /* not hard enough! */

		if( IsExtraStage() )
		{
			if( STATSMAN->m_CurStageStats.m_player[pn].GetGrade() <= GRADE_TIER_FOR_EXTRA_2 )
				return true;
		}
		else if( STATSMAN->m_CurStageStats.m_player[pn].GetGrade() <= GRADE_TIER_FOR_EXTRA_1 )
		{
			return true;
		}
	}
	
	return false;
}

PlayerNumber GameState::GetBestPlayer() const
{
	FOREACH_PlayerNumber( pn )
		if( GetStageResult(pn) == RESULT_WIN )
			return pn;
	return PLAYER_INVALID;	// draw
}

StageResult GameState::GetStageResult( PlayerNumber pn ) const
{
	switch( m_PlayMode )
	{
	case PLAY_MODE_BATTLE:
	case PLAY_MODE_RAVE:
		if( fabsf(m_fTugLifePercentP1 - 0.5f) < 0.0001f )
			return RESULT_DRAW;
		switch( pn )
		{
		case PLAYER_1:	return (m_fTugLifePercentP1>=0.5f)?RESULT_WIN:RESULT_LOSE;
		case PLAYER_2:	return (m_fTugLifePercentP1<0.5f)?RESULT_WIN:RESULT_LOSE;
		default:	ASSERT(0); return RESULT_LOSE;
		}
	}

	StageResult win = RESULT_WIN;
	FOREACH_PlayerNumber( p )
	{
		if( p == pn )
			continue;

		/* If anyone did just as well, at best it's a draw. */
		if( STATSMAN->m_CurStageStats.m_player[p].m_iActualDancePoints == STATSMAN->m_CurStageStats.m_player[pn].m_iActualDancePoints )
			win = RESULT_DRAW;

		/* If anyone did better, we lost. */
		if( STATSMAN->m_CurStageStats.m_player[p].m_iActualDancePoints > STATSMAN->m_CurStageStats.m_player[pn].m_iActualDancePoints )
			return RESULT_LOSE;
	}
	return win;
}



void GameState::GetDefaultPlayerOptions( PlayerOptions &po )
{
	po.Init();
	po.FromString( PREFSMAN->m_sDefaultModifiers );
	po.FromString( CommonMetrics::DEFAULT_MODIFIERS );
	if( po.m_sNoteSkin.empty() )
		po.m_sNoteSkin = CommonMetrics::DEFAULT_NOTESKIN_NAME;
}

void GameState::GetDefaultSongOptions( SongOptions &so )
{
	so.Init();
	so.FromString( PREFSMAN->m_sDefaultModifiers );
	so.FromString( CommonMetrics::DEFAULT_MODIFIERS );
}

void GameState::ResetToDefaultSongOptions( ModsLevel l )
{
	SongOptions so;
	GetDefaultSongOptions( so );
	m_SongOptions.Assign( l, so );
}

void GameState::ApplyPreferredModifiers( PlayerNumber pn, RString sModifiers )
{
	m_pPlayerState[pn]->m_PlayerOptions.FromString( ModsLevel_Preferred, sModifiers );
	m_SongOptions.FromString( ModsLevel_Preferred, sModifiers );
}

void GameState::ApplyStageModifiers( PlayerNumber pn, RString sModifiers )
{
	m_pPlayerState[pn]->m_PlayerOptions.FromString( ModsLevel_Stage, sModifiers );
	m_SongOptions.FromString( ModsLevel_Stage, sModifiers );
}

bool GameState::CurrentOptionsDisqualifyPlayer( PlayerNumber pn )
{
	if( !PREFSMAN->m_bDisqualification )
		return false;

	if( !IsHumanPlayer(pn) )
		return false;

	const PlayerOptions &po = m_pPlayerState[pn]->m_PlayerOptions.GetPreferred();

	// Check the stored player options for disqualify.  Don't disqualify because
	// of mods that were forced.
	if( IsCourseMode() )
		return po.IsEasierForCourseAndTrail(  m_pCurCourse, m_pCurTrail[pn] );
	else
		return po.IsEasierForSongAndSteps(  m_pCurSong, m_pCurSteps[pn], pn);
}

void GameState::GetAllUsedNoteSkins( vector<RString> &out ) const
{
	FOREACH_EnabledPlayer( pn )
	{
		out.push_back( m_pPlayerState[pn]->m_PlayerOptions.GetCurrent().m_sNoteSkin );

		/* Add note skins that are used in courses. */
		if( IsCourseMode() )
		{
			const Trail *pTrail = m_pCurTrail[pn];
			ASSERT( pTrail );

			FOREACH_CONST( TrailEntry, pTrail->m_vEntries, e )
			{
				PlayerOptions po;
				po.FromString( e->Modifiers );
				if( !po.m_sNoteSkin.empty() )
					out.push_back( po.m_sNoteSkin );
			}
		}
	}

	/* Remove duplicates. */
	sort( out.begin(), out.end() );
	out.erase( unique( out.begin(), out.end() ), out.end() );
}

void GameState::RemoveAllActiveAttacks()	// called on end of song
{
	FOREACH_PlayerNumber( p )
		m_pPlayerState[p]->RemoveActiveAttacks();
}

template<class T>
void setmin( T &a, const T &b )
{
	a = min(a, b);
}

template<class T>
void setmax( T &a, const T &b )
{
	a = max(a, b);
}

PlayerOptions::FailType GameState::GetPlayerFailType( const PlayerState *pPlayerState ) const
{
	PlayerNumber pn = pPlayerState->m_PlayerNumber;
	PlayerOptions::FailType ft = pPlayerState->m_PlayerOptions.GetCurrent().m_FailType;

	/* If the player changed the fail mode explicitly, leave it alone. */
	if( m_bChangedFailTypeOnScreenSongOptions )
		return ft;

	if( IsCourseMode() )
	{
		if( PREFSMAN->m_bMinimum1FullSongInCourses && GetCourseSongIndex()==0 )
			ft = max( ft, PlayerOptions::FAIL_IMMEDIATE_CONTINUE );	// take the least harsh of the two FailTypes
	}
	else
	{
		Difficulty dc = Difficulty_Invalid;
		if( m_pCurSteps[pn] )
			dc = m_pCurSteps[pn]->GetDifficulty();

		bool bFirstStage = false;
		if( !IsEventMode() )
			bFirstStage |= m_iPlayerStageTokens[pPlayerState->m_PlayerNumber] == PREFSMAN->m_iSongsPerPlay-1; // HACK; -1 because this is called during gameplay

		/* Easy and beginner are never harder than FAIL_IMMEDIATE_CONTINUE. */
		if( dc <= Difficulty_Easy )
			setmax( ft, PlayerOptions::FAIL_IMMEDIATE_CONTINUE );

		if( dc <= Difficulty_Easy && bFirstStage && PREFSMAN->m_bFailOffForFirstStageEasy )
			setmax( ft, PlayerOptions::FAIL_OFF );

		/* If beginner's steps were chosen, and this is the first stage,
		 * turn off failure completely. */
		if( dc == Difficulty_Beginner && bFirstStage )
			setmax( ft, PlayerOptions::FAIL_OFF );

		if( dc == Difficulty_Beginner && PREFSMAN->m_bFailOffInBeginner )
			setmax( ft, PlayerOptions::FAIL_OFF );
	}

	return ft;
}

bool GameState::ShowW1() const
{
	switch( PREFSMAN->m_AllowW1 )
	{
	case ALLOW_W1_NEVER:		return false;
	case ALLOW_W1_COURSES_ONLY:	return IsCourseMode();
	case ALLOW_W1_EVERYWHERE:	return true;
	default:	ASSERT(0);
	}
}


static ThemeMetric<bool> PERSONAL_RECORD_FEATS("GameState","PersonalRecordFeats");
static ThemeMetric<bool> CATEGORY_RECORD_FEATS("GameState","CategoryRecordFeats");
void GameState::GetRankingFeats( PlayerNumber pn, vector<RankingFeat> &asFeatsOut ) const
{
	if( !IsHumanPlayer(pn) )
		return;

	Profile *pProf = PROFILEMAN->GetProfile(pn);

	// Check for feats even if the PlayMode is rave or battle because the player may have
	// made high scores then switched modes.
	CHECKPOINT_M( ssprintf("PlayMode %i", int(m_PlayMode)) );
	switch( m_PlayMode )
	{
	case PLAY_MODE_REGULAR:
	case PLAY_MODE_BATTLE:
	case PLAY_MODE_RAVE:
		{
			CHECKPOINT;

			StepsType st = GetCurrentStyle()->m_StepsType;

			//
			// Find unique Song and Steps combinations that were played.
			// We must keep only the unique combination or else we'll double-count
			// high score markers.
			//
			vector<SongAndSteps> vSongAndSteps;

			for( unsigned i=0; i<STATSMAN->m_vPlayedStageStats.size(); i++ )
			{
				CHECKPOINT_M( ssprintf("%u/%i", i, (int)STATSMAN->m_vPlayedStageStats.size() ) );
				SongAndSteps sas;
				ASSERT( !STATSMAN->m_vPlayedStageStats[i].m_vpPlayedSongs.empty() );
				sas.pSong = STATSMAN->m_vPlayedStageStats[i].m_vpPlayedSongs[0];
				ASSERT( sas.pSong );
				if( STATSMAN->m_vPlayedStageStats[i].m_player[pn].m_vpPossibleSteps.empty() )
					continue;
				sas.pSteps = STATSMAN->m_vPlayedStageStats[i].m_player[pn].m_vpPossibleSteps[0];
				ASSERT( sas.pSteps );
				vSongAndSteps.push_back( sas );
			}
			CHECKPOINT;

			sort( vSongAndSteps.begin(), vSongAndSteps.end() );

			vector<SongAndSteps>::iterator toDelete = unique( vSongAndSteps.begin(), vSongAndSteps.end() );
			vSongAndSteps.erase(toDelete, vSongAndSteps.end());

			CHECKPOINT;
			for( unsigned i=0; i<vSongAndSteps.size(); i++ )
			{
				Song* pSong = vSongAndSteps[i].pSong;
				Steps* pSteps = vSongAndSteps[i].pSteps;

				// Find Machine Records
				{
					HighScoreList &hsl = PROFILEMAN->GetMachineProfile()->GetStepsHighScoreList(pSong,pSteps);
					for( unsigned j=0; j<hsl.vHighScores.size(); j++ )
					{
						HighScore &hs = hsl.vHighScores[j];

						if( hs.GetName() != RANKING_TO_FILL_IN_MARKER[pn] )
							continue;

						RankingFeat feat;
						feat.Type = RankingFeat::SONG;
						feat.pSong = pSong;
						feat.pSteps = pSteps;
						feat.Feat = ssprintf("MR #%d in %s %s", j+1, pSong->GetTranslitMainTitle().c_str(), DifficultyToString(pSteps->GetDifficulty()).c_str() );
						feat.pStringToFill = hs.GetNameMutable();
						feat.grade = hs.GetGrade();
						feat.fPercentDP = hs.GetPercentDP();
						feat.iScore = hs.GetScore();

						if( pSong->HasBanner() )
							feat.Banner = pSong->GetBannerPath();

						asFeatsOut.push_back( feat );
					}
				}
		
				// Find Personal Records
				if( pProf && PERSONAL_RECORD_FEATS )
				{
					HighScoreList &hsl = pProf->GetStepsHighScoreList(pSong,pSteps);
					for( unsigned j=0; j<hsl.vHighScores.size(); j++ )
					{
						HighScore &hs = hsl.vHighScores[j];

						if( hs.GetName() != RANKING_TO_FILL_IN_MARKER[pn] )
							continue;

						RankingFeat feat;
						feat.pSong = pSong;
						feat.pSteps = pSteps;
						feat.Type = RankingFeat::SONG;
						feat.Feat = ssprintf("PR #%d in %s %s", j+1, pSong->GetTranslitMainTitle().c_str(), DifficultyToString(pSteps->GetDifficulty()).c_str() );
						feat.pStringToFill = hs.GetNameMutable();
						feat.grade = hs.GetGrade();
						feat.fPercentDP = hs.GetPercentDP();
						feat.iScore = hs.GetScore();

						// XXX: temporary hack
						if( pSong->HasBackground() )
							feat.Banner = pSong->GetBackgroundPath();
		//					if( pSong->HasBanner() )
		//						feat.Banner = pSong->GetBannerPath();

						asFeatsOut.push_back( feat );
					}
				}
			}

			CHECKPOINT;
			StageStats stats;
			STATSMAN->GetFinalEvalStageStats( stats );


			// Find Machine Category Records
			FOREACH_ENUM( RankingCategory, rc )
			{
				if( !CATEGORY_RECORD_FEATS )
					continue;
				HighScoreList &hsl = PROFILEMAN->GetMachineProfile()->GetCategoryHighScoreList( st, rc );
				for( unsigned j=0; j<hsl.vHighScores.size(); j++ )
				{
					HighScore &hs = hsl.vHighScores[j];
					if( hs.GetName() != RANKING_TO_FILL_IN_MARKER[pn] )
						continue;

					RankingFeat feat;
					feat.Type = RankingFeat::CATEGORY;
					feat.Feat = ssprintf("MR #%d in Type %c (%d)", j+1, 'A'+rc, stats.GetAverageMeter(pn) );
					feat.pStringToFill = hs.GetNameMutable();
					feat.grade = Grade_NoData;
					feat.iScore = hs.GetScore();
					feat.fPercentDP = hs.GetPercentDP();
					asFeatsOut.push_back( feat );
				}
			}

			// Find Personal Category Records
			FOREACH_ENUM( RankingCategory, rc )
			{
				if( !CATEGORY_RECORD_FEATS )
					continue;

				if( pProf && PERSONAL_RECORD_FEATS )
				{
					HighScoreList &hsl = pProf->GetCategoryHighScoreList( st, rc );
					for( unsigned j=0; j<hsl.vHighScores.size(); j++ )
					{
						HighScore &hs = hsl.vHighScores[j];
						if( hs.GetName() != RANKING_TO_FILL_IN_MARKER[pn] )
							continue;

						RankingFeat feat;
						feat.Type = RankingFeat::CATEGORY;
						feat.Feat = ssprintf("PR #%d in Type %c (%d)", j+1, 'A'+rc, stats.GetAverageMeter(pn) );
						feat.pStringToFill = hs.GetNameMutable();
						feat.grade = Grade_NoData;
						feat.iScore = hs.GetScore();
						feat.fPercentDP = hs.GetPercentDP();
						asFeatsOut.push_back( feat );
					}
				}
			}
		}
		break;
	case PLAY_MODE_NONSTOP:
	case PLAY_MODE_ONI:
	case PLAY_MODE_ENDLESS:
		{
			CHECKPOINT;
			Course* pCourse = m_pCurCourse;
			ASSERT( pCourse );
			Trail *pTrail = m_pCurTrail[pn];
			ASSERT( pTrail );
			CourseDifficulty cd = pTrail->m_CourseDifficulty;

			// Find Machine Records
			{
				Profile* pProfile = PROFILEMAN->GetMachineProfile();
				HighScoreList &hsl = pProfile->GetCourseHighScoreList( pCourse, pTrail );
				for( unsigned i=0; i<hsl.vHighScores.size(); i++ )
				{
					HighScore &hs = hsl.vHighScores[i];
					if( hs.GetName() != RANKING_TO_FILL_IN_MARKER[pn] )
							continue;

					RankingFeat feat;
					feat.Type = RankingFeat::COURSE;
					feat.pCourse = pCourse;
					feat.Feat = ssprintf("MR #%d in %s", i+1, pCourse->GetDisplayFullTitle().c_str() );
					if( cd != Difficulty_Medium )
						feat.Feat += " " + CourseDifficultyToLocalizedString(cd);
					feat.pStringToFill = hs.GetNameMutable();
					feat.grade = Grade_NoData;
					feat.iScore = hs.GetScore();
					feat.fPercentDP = hs.GetPercentDP();
					if( pCourse->HasBanner() )
						feat.Banner = pCourse->GetBannerPath();
					asFeatsOut.push_back( feat );
				}
			}

			// Find Personal Records
			if( PERSONAL_RECORD_FEATS && PROFILEMAN->IsPersistentProfile(pn) )
			{
				HighScoreList &hsl = pProf->GetCourseHighScoreList( pCourse, pTrail );
				for( unsigned i=0; i<hsl.vHighScores.size(); i++ )
				{
					HighScore& hs = hsl.vHighScores[i];
					if( hs.GetName() != RANKING_TO_FILL_IN_MARKER[pn] )
							continue;

					RankingFeat feat;
					feat.Type = RankingFeat::COURSE;
					feat.pCourse = pCourse;
					feat.Feat = ssprintf("PR #%d in %s", i+1, pCourse->GetDisplayFullTitle().c_str() );
					feat.pStringToFill = hs.GetNameMutable();
					feat.grade = Grade_NoData;
					feat.iScore = hs.GetScore();
					feat.fPercentDP = hs.GetPercentDP();
					if( pCourse->HasBanner() )
						feat.Banner = pCourse->GetBannerPath();
					asFeatsOut.push_back( feat );
				}
			}
		}
		break;
	default:
		ASSERT(0);
	}
}

bool GameState::AnyPlayerHasRankingFeats() const
{
	vector<RankingFeat> vFeats;
	FOREACH_PlayerNumber( p )
	{
		GetRankingFeats( p, vFeats );
		if( !vFeats.empty() )
			return true;
	}
	return false;
}

void GameState::StoreRankingName( PlayerNumber pn, RString sName )
{
	sName.MakeUpper();

	if( USE_NAME_BLACKLIST )
	{
		RageFile file;
		if( file.Open(NAME_BLACKLIST_FILE) )
		{
			RString sLine;
			
			while( !file.AtEOF() )
			{
				if( file.GetLine(sLine) == -1 )
				{
					LOG->Warn( "Error reading \"%s\": %s", NAME_BLACKLIST_FILE, file.GetError().c_str() );
					break;
				}

				sLine.MakeUpper();
				if( !sLine.empty() && sName.find(sLine) != string::npos )	// name contains a bad word
				{
					LOG->Trace( "entered '%s' matches blacklisted item '%s'", sName.c_str(), sLine.c_str() );
					sName = "";
					break;
				}
			}
		}
	}

	vector<RankingFeat> aFeats;
	GetRankingFeats( pn, aFeats );

	for( unsigned i=0; i<aFeats.size(); i++ )
	{
		*aFeats[i].pStringToFill = sName;

		// save name pointers as we fill them
		m_vpsNamesThatWereFilled.push_back( aFeats[i].pStringToFill );
	}


	Profile *pProfile = PROFILEMAN->GetMachineProfile();

	if( !PREFSMAN->m_bAllowMultipleHighScoreWithSameName )
	{
		//
		// erase all but the highest score for each name
		//
		FOREACHM( SongID, Profile::HighScoresForASong, pProfile->m_SongHighScores, iter )
			FOREACHM( StepsID, Profile::HighScoresForASteps, iter->second.m_StepsHighScores, iter2 )
				iter2->second.hsl.RemoveAllButOneOfEachName();

		FOREACHM( CourseID, Profile::HighScoresForACourse, pProfile->m_CourseHighScores, iter )
			FOREACHM( TrailID, Profile::HighScoresForATrail, iter->second.m_TrailHighScores, iter2 )
				iter2->second.hsl.RemoveAllButOneOfEachName();
	}

	//
	// clamp high score sizes
	//
	FOREACHM( SongID, Profile::HighScoresForASong, pProfile->m_SongHighScores, iter )
		FOREACHM( StepsID, Profile::HighScoresForASteps, iter->second.m_StepsHighScores, iter2 )
			iter2->second.hsl.ClampSize( true );

	FOREACHM( CourseID, Profile::HighScoresForACourse, pProfile->m_CourseHighScores, iter )
		FOREACHM( TrailID, Profile::HighScoresForATrail, iter->second.m_TrailHighScores, iter2 )
			iter2->second.hsl.ClampSize( true );
}

bool GameState::AllAreInDangerOrWorse() const
{
	FOREACH_EnabledPlayer( p )
		if( m_pPlayerState[p]->m_HealthState < HealthState_Danger )
			return false;
	return true;
}

bool GameState::OneIsHot() const
{
	FOREACH_EnabledPlayer( p )
		if( m_pPlayerState[p]->m_HealthState == HealthState_Hot )
			return true;
	return false;
}

bool GameState::IsTimeToPlayAttractSounds() const
{
	// m_iNumTimesThroughAttract will be -1 from the first attract screen after 
	// the end of a game until the next time FIRST_ATTRACT_SCREEN is reached.
	// Play attract sounds for this sort span of time regardless of 
	// m_AttractSoundFrequency because it's awkward to have the machine go 
	// silent immediately after the end of a game.
	if( m_iNumTimesThroughAttract == -1 )
		return true;

	if( PREFSMAN->m_AttractSoundFrequency == ASF_NEVER )
		return false;

	// play attract sounds once every m_iAttractSoundFrequency times through
	if( (m_iNumTimesThroughAttract % PREFSMAN->m_AttractSoundFrequency)==0 )
		return true;

	return false;
}

void GameState::VisitAttractScreen( const RString sScreenName )
{
	if( sScreenName == CommonMetrics::FIRST_ATTRACT_SCREEN.GetValue() )
		m_iNumTimesThroughAttract++;
}

bool GameState::DifficultiesLocked() const
{
 	if( m_PlayMode == PLAY_MODE_RAVE )
		return true;
	if( IsCourseMode() )
		return PREFSMAN->m_bLockCourseDifficulties;
 	if( GetCurrentStyle()->m_bLockDifficulties )
		return true;
	return false;
}

bool GameState::ChangePreferredDifficultyAndStepsType( PlayerNumber pn, Difficulty dc, StepsType st )
{
	m_PreferredDifficulty[pn].Set( dc );
	m_PreferredStepsType.Set( st );
	if( DifficultiesLocked() )
		FOREACH_PlayerNumber( p )
			if( p != pn )
				m_PreferredDifficulty[p].Set( m_PreferredDifficulty[pn] );

	return true;
}

/* When only displaying difficulties in DIFFICULTIES_TO_SHOW, use GetClosestShownDifficulty
 * to find which difficulty to show, and ChangePreferredDifficulty(pn, dir) to change
 * difficulty. */
bool GameState::ChangePreferredDifficulty( PlayerNumber pn, int dir )
{
	const vector<Difficulty> &v = CommonMetrics::DIFFICULTIES_TO_SHOW.GetValue();

	Difficulty d = GetClosestShownDifficulty(pn);
	while( 1 )
	{
		d = enum_add2( d, dir );
		if( d < 0 || d >= NUM_Difficulty )
			return false;
		if( find(v.begin(), v.end(), d) != v.end() )
			break; // found
	}

	m_PreferredDifficulty[pn].Set( d );
	return true;
}

/* The user may be set to prefer a difficulty that isn't always shown; typically,
 * Difficulty_Edit.  Return the closest shown difficulty <= m_PreferredDifficulty. */
Difficulty GameState::GetClosestShownDifficulty( PlayerNumber pn ) const
{
	const vector<Difficulty> &v = CommonMetrics::DIFFICULTIES_TO_SHOW.GetValue();

	Difficulty iClosest = (Difficulty) 0;
	int iClosestDist = -1;
	FOREACH_CONST( Difficulty, v, dc )
	{
		int iDist = m_PreferredDifficulty[pn] - *dc;
		if( iDist < 0 )
			continue;
		if( iClosestDist != -1 && iDist > iClosestDist )
			continue;
		iClosestDist = iDist;
		iClosest = *dc;
	}

	return iClosest;
}

bool GameState::ChangePreferredCourseDifficultyAndStepsType( PlayerNumber pn, CourseDifficulty cd, StepsType st )
{
	m_PreferredCourseDifficulty[pn].Set( cd );
	m_PreferredStepsType.Set( st );
	if( PREFSMAN->m_bLockCourseDifficulties )
		FOREACH_PlayerNumber( p )
			if( p != pn )
				m_PreferredCourseDifficulty[p].Set( m_PreferredCourseDifficulty[pn] );

	return true;
}

bool GameState::ChangePreferredCourseDifficulty( PlayerNumber pn, int dir )
{
	/* If we have a course selected, only choose among difficulties available in the course. */
	const Course *pCourse = m_pCurCourse;

	const vector<CourseDifficulty> &v = CommonMetrics::COURSE_DIFFICULTIES_TO_SHOW.GetValue();

	CourseDifficulty cd = m_PreferredCourseDifficulty[pn];
	while( 1 )
	{
		cd = enum_add2( cd, dir );
		if( cd < 0 || cd >= NUM_Difficulty )
			return false;
		if( find(v.begin(),v.end(),cd) == v.end() )
			continue; /* not available */
		if( !pCourse || pCourse->GetTrail( GetCurrentStyle()->m_StepsType, cd ) )
			break;
	}

	return ChangePreferredCourseDifficulty( pn, cd );
}

bool GameState::IsCourseDifficultyShown( CourseDifficulty cd )
{
	const vector<CourseDifficulty> &v = CommonMetrics::COURSE_DIFFICULTIES_TO_SHOW.GetValue();
	return find(v.begin(), v.end(), cd) != v.end();
}

Difficulty GameState::GetEasiestStepsDifficulty() const
{
	Difficulty dc = Difficulty_Invalid;
	FOREACH_HumanPlayer( p )
	{
		if( m_pCurSteps[p] == NULL )
		{
			LOG->Warn( "GetEasiestNotesDifficulty called but p%i hasn't chosen notes", p+1 );
			continue;
		}
		dc = min( dc, m_pCurSteps[p]->GetDifficulty() );
	}
	return dc;
}

bool GameState::IsEventMode() const
{
	return m_bTemporaryEventMode || PREFSMAN->m_bEventMode; 
}

CoinMode GameState::GetCoinMode() const
{
	if( IsEventMode() && GamePreferences::m_CoinMode == CoinMode_Pay )
		return CoinMode_Free; 
	else 
		return GamePreferences::m_CoinMode;
}

Premium	GameState::GetPremium() const
{ 
	if( IsEventMode() ) 
		return Premium_Off; 
	else 
		return g_Premium; 
}

float GameState::GetGoalPercentComplete( PlayerNumber pn )
{
	const Profile *pProfile = PROFILEMAN->GetProfile(pn);
	const StageStats &ssCurrent = STATSMAN->m_CurStageStats;
	const PlayerStageStats &pssCurrent = ssCurrent.m_player[pn];

	float fActual = 0;
	float fGoal = 0;
	switch( pProfile->m_GoalType )
	{
	case GoalType_Calories:
		fActual = pssCurrent.m_fCaloriesBurned;
		fGoal = (float)pProfile->m_iGoalCalories;
		break;
	case GoalType_Time:
		fActual = ssCurrent.m_fGameplaySeconds;
		fGoal = (float)pProfile->m_iGoalSeconds;
		break;
	case GoalType_None:
		return 0;	// never complete
	default:
		ASSERT(0);
	}
	if( fGoal == 0 )
		return 0;
	else
		return fActual / fGoal;
}

bool GameState::PlayerIsUsingModifier( PlayerNumber pn, const RString &sModifier )
{
	PlayerOptions po = m_pPlayerState[pn]->m_PlayerOptions.GetCurrent();
	SongOptions so = m_SongOptions.GetCurrent();
	po.FromString( sModifier );
	so.FromString( sModifier );

	return po == m_pPlayerState[pn]->m_PlayerOptions.GetCurrent()  &&  so == m_SongOptions.GetCurrent();
}

Profile* GameState::GetEditLocalProfile()
{
	if( m_sEditLocalProfileID.Get().empty() )
		return NULL;
	return PROFILEMAN->GetLocalProfile( m_sEditLocalProfileID );
}


PlayerNumber GetNextHumanPlayer( PlayerNumber pn )
{
	for( enum_add(pn, 1); pn < NUM_PLAYERS; enum_add(pn, 1) )
		if( GAMESTATE->IsHumanPlayer(pn) )
			return pn;
	return PLAYER_INVALID;
}

PlayerNumber GetNextEnabledPlayer( PlayerNumber pn )
{
	for( enum_add(pn, 1); pn < NUM_PLAYERS; enum_add(pn, 1) )
		if( GAMESTATE->IsPlayerEnabled(pn) )
			return pn;
	return PLAYER_INVALID;
}

PlayerNumber GetNextCpuPlayer( PlayerNumber pn )
{
	for( enum_add(pn, 1); pn < NUM_PLAYERS; enum_add(pn, 1) )
		if( GAMESTATE->IsCpuPlayer(pn) )
			return pn;
	return PLAYER_INVALID;
}

PlayerNumber GetNextPotentialCpuPlayer( PlayerNumber pn )
{
	for( enum_add(pn, 1); pn < NUM_PLAYERS; enum_add(pn, 1) )
		if( !GAMESTATE->IsHumanPlayer(pn) )
			return pn;
	return PLAYER_INVALID;
}

MultiPlayer GetNextEnabledMultiPlayer( MultiPlayer mp )
{
	for( enum_add(mp, 1); mp < NUM_MultiPlayer; enum_add(mp, 1) )
		if( GAMESTATE->IsMultiPlayerEnabled(mp) )
			return mp;
	return MultiPlayer_Invalid;
}



// lua start
#include "LuaBinding.h"
#include "Game.h"

class LunaGameState: public Luna<GameState>
{
public:
	DEFINE_METHOD( IsPlayerEnabled,			IsPlayerEnabled(Enum::Check<PlayerNumber>(L, 1)) )
	DEFINE_METHOD( IsHumanPlayer,			IsHumanPlayer(Enum::Check<PlayerNumber>(L, 1)) )
	DEFINE_METHOD( GetPlayerDisplayName,		GetPlayerDisplayName(Enum::Check<PlayerNumber>(L, 1)) )
	DEFINE_METHOD( GetMasterPlayerNumber,		m_MasterPlayerNumber )
	DEFINE_METHOD( GetMultiplayer,			m_bMultiplayer )
	DEFINE_METHOD( GetNumMultiplayerNoteFields,	m_iNumMultiplayerNoteFields )
	static int SetNumMultiplayerNoteFields( T* p, lua_State *L )
	{
		p->m_iNumMultiplayerNoteFields = IArg(1);
		return 0;
	}
	static int GetPlayerState( T* p, lua_State *L )
	{ 
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		p->m_pPlayerState[pn]->PushSelf(L);
		return 1;
	}
	static int GetMultiPlayerState( T* p, lua_State *L )
	{ 
		MultiPlayer mp = Enum::Check<MultiPlayer>(L, 1);
		p->m_pMultiPlayerState[mp]->PushSelf(L);
		return 1;
	}
	static int ApplyGameCommand( T* p, lua_State *L )
	{
		PlayerNumber pn = PLAYER_INVALID;
		if( lua_gettop(L) >= 2 && !lua_isnil(L,2) )
			pn = Enum::Check<PlayerNumber>(L, 2);
		p->ApplyGameCommand(SArg(1),pn);
		return 0;
	}
	static int GetCurrentSong( T* p, lua_State *L )			{ if(p->m_pCurSong) p->m_pCurSong->PushSelf(L); else lua_pushnil(L); return 1; }
	static int SetCurrentSong( T* p, lua_State *L )
	{ 
		if( lua_isnil(L,1) ) { p->m_pCurSong.Set( NULL ); }
		else { Song *pS = Luna<Song>::check( L, 1, true ); p->m_pCurSong.Set( pS ); }
		return 0;
	}
	static int GetCurrentSteps( T* p, lua_State *L )
	{
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		Steps *pSteps = p->m_pCurSteps[pn];  
		if( pSteps ) { pSteps->PushSelf(L); }
		else		 { lua_pushnil(L); }
		return 1;
	}
	static int SetCurrentSteps( T* p, lua_State *L )
	{ 
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		if( lua_isnil(L,2) )	{ p->m_pCurSteps[pn].Set( NULL ); }
		else					{ Steps *pS = Luna<Steps>::check(L,2); p->m_pCurSteps[pn].Set( pS ); }
	
		// Why Broadcast again?  This is double-broadcasting. -Chris
		MESSAGEMAN->Broadcast( (MessageID)(Message_CurrentStepsP1Changed+pn) );
		return 0;
	}
	static int GetCurrentCourse( T* p, lua_State *L )		{ if(p->m_pCurCourse) p->m_pCurCourse->PushSelf(L); else lua_pushnil(L); return 1; }
	static int SetCurrentCourse( T* p, lua_State *L )
	{ 
		if( lua_isnil(L,1) ) { p->m_pCurCourse.Set( NULL ); }
		else { Course *pC = Luna<Course>::check(L,1); p->m_pCurCourse.Set( pC ); }
		return 0;
	}
	static int GetCurrentTrail( T* p, lua_State *L )
	{
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		Trail *pTrail = p->m_pCurTrail[pn];  
		if( pTrail ) { pTrail->PushSelf(L); }
		else		 { lua_pushnil(L); }
		return 1;
	}
	static int SetCurrentTrail( T* p, lua_State *L )
	{ 
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		if( lua_isnil(L,2) )	{ p->m_pCurTrail[pn].Set( NULL ); }
		else					{ Trail *pS = Luna<Trail>::check(L,2); p->m_pCurTrail[pn].Set( pS ); }
		MESSAGEMAN->Broadcast( (MessageID)(Message_CurrentTrailP1Changed+pn) );
		return 0;
	}
	static int GetPreferredSong( T* p, lua_State *L )		{ if(p->m_pPreferredSong) p->m_pPreferredSong->PushSelf(L); else lua_pushnil(L); return 1; }
	static int SetPreferredSong( T* p, lua_State *L )
	{
		if( lua_isnil(L,1) ) { p->m_pPreferredSong = NULL; }
		else { Song *pS = Luna<Song>::check(L,1); p->m_pPreferredSong = pS; }
		return 0;
	}
	static int SetTemporaryEventMode( T* p, lua_State *L )	{ p->m_bTemporaryEventMode = BArg(1); return 0; }
	static int Env( T* p, lua_State *L )	{ p->m_Environment->PushSelf(L); return 1; }
	static int GetEditSourceSteps( T* p, lua_State *L )
	{
		Steps *pSteps = p->m_pEditSourceSteps;  
		if( pSteps ) { pSteps->PushSelf(L); }
		else		 { lua_pushnil(L); }
		return 1;
	}
	static int SetPreferredDifficulty( T* p, lua_State *L )
	{
		PlayerNumber pn = Enum::Check<PlayerNumber>( L, 1 );
		Difficulty dc = Enum::Check<Difficulty>( L, 2 );
		p->m_PreferredDifficulty[pn].Set( dc );
		return 0;
	}
	DEFINE_METHOD( GetPreferredDifficulty,		m_PreferredDifficulty[Enum::Check<PlayerNumber>(L, 1)] )
	DEFINE_METHOD( AnyPlayerHasRankingFeats,	AnyPlayerHasRankingFeats() )
	DEFINE_METHOD( IsCourseMode,			IsCourseMode() )
	DEFINE_METHOD( IsDemonstration,			m_bDemonstrationOrJukebox )
	DEFINE_METHOD( GetPlayMode,			m_PlayMode )
	DEFINE_METHOD( GetSortOrder,			m_SortOrder )
	DEFINE_METHOD( GetStageIndex,			m_iCurrentStageIndex )
	DEFINE_METHOD( IsGoalComplete,			IsGoalComplete(Enum::Check<PlayerNumber>(L, 1)) )
	DEFINE_METHOD( PlayerIsUsingModifier,		PlayerIsUsingModifier(Enum::Check<PlayerNumber>(L, 1), SArg(2)) )
	DEFINE_METHOD( GetCourseSongIndex,		GetCourseSongIndex() )
	DEFINE_METHOD( GetLoadingCourseSongIndex,	GetLoadingCourseSongIndex() )
	DEFINE_METHOD( GetSmallestNumStagesLeftForAnyHumanPlayer, GetSmallestNumStagesLeftForAnyHumanPlayer() )
	DEFINE_METHOD( IsAnExtraStage,			IsAnExtraStage() )
	DEFINE_METHOD( IsExtraStage,			IsExtraStage() )
	DEFINE_METHOD( IsExtraStage2,			IsExtraStage2() )
	DEFINE_METHOD( GetCurrentStage,			GetCurrentStage() )
	DEFINE_METHOD( HasEarnedExtraStage,		HasEarnedExtraStage() )
	DEFINE_METHOD( GetEasiestStepsDifficulty,	GetEasiestStepsDifficulty() )
	DEFINE_METHOD( IsEventMode,			IsEventMode() )
	DEFINE_METHOD( GetNumPlayersEnabled,		GetNumPlayersEnabled() )
	DEFINE_METHOD( GetSongBeat,			m_fSongBeat )
	DEFINE_METHOD( GetSongBeatVisible,		m_fSongBeatVisible )
	DEFINE_METHOD( GetSongBPS,			m_fCurBPS )
	DEFINE_METHOD( GetSongFreeze,			m_bFreeze )
	DEFINE_METHOD( GetGameplayLeadIn,		m_bGameplayLeadIn )
	DEFINE_METHOD( GetCoins,			m_iCoins )
	DEFINE_METHOD( IsSideJoined,			m_bSideIsJoined[Enum::Check<PlayerNumber>(L, 1)] )
	DEFINE_METHOD( GetCoinsNeededToJoin,		GetCoinsNeededToJoin() )
	DEFINE_METHOD( EnoughCreditsToJoin,		EnoughCreditsToJoin() )
	DEFINE_METHOD( PlayersCanJoin,			PlayersCanJoin() )
	DEFINE_METHOD( GetNumSidesJoined,		GetNumSidesJoined() )
	DEFINE_METHOD( GetCoinMode,			GetCoinMode() )
	DEFINE_METHOD( GetPremium,			GetPremium() )
	DEFINE_METHOD( GetSongOptionsString,		m_SongOptions.GetCurrent().GetString() )
	static int GetSongOptions( T* p, lua_State *L )
	{
		ModsLevel m = Enum::Check<ModsLevel>( L, 1 );
		RString s = p->m_SongOptions.Get(m).GetString();
		LuaHelpers::Push( L, s );
		return 1;
	}
	static int GetDefaultSongOptions( T* p, lua_State *L )
	{
		SongOptions so;
		p->GetDefaultSongOptions( so );
		lua_pushstring(L, so.GetString());
		return 1;
	}
	static int SetSongOptions( T* p, lua_State *L )
	{
		ModsLevel m = Enum::Check<ModsLevel>( L, 1 );

		SongOptions so;
		
		so.FromString( SArg(2) );
		p->m_SongOptions.Assign( m, so );
		return 0;
	}
	static int IsWinner( T* p, lua_State *L )
	{
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		lua_pushboolean(L, p->GetStageResult(pn)==RESULT_WIN); return 1;
	}
	static int IsDraw( T* p, lua_State *L )
	{
		lua_pushboolean(L, p->GetStageResult(PLAYER_1)==RESULT_DRAW); return 1;
	}
	static int GetCurrentGame( T* p, lua_State *L )			{ const_cast<Game*>(p->GetCurrentGame())->PushSelf( L ); return 1; }
	DEFINE_METHOD( GetEditCourseEntryIndex,		m_iEditCourseEntryIndex )
	DEFINE_METHOD( GetEditLocalProfileID,		m_sEditLocalProfileID.Get() )
	static int GetEditLocalProfile( T* p, lua_State *L )
	{
		Profile *pProfile = p->GetEditLocalProfile();
		if( pProfile )
			pProfile->PushSelf(L);
		else
			lua_pushnil( L );
		return 1;
	}

	static int GetCurrentStepsCredits( T* p, lua_State *L )
	{
		const Song* pSong = p->m_pCurSong;
		if( pSong == NULL )
			return 0;

		// use a vector and not a set so that ordering is maintained
		vector<const Steps*> vpStepsToShow;
		FOREACH_HumanPlayer( p )
		{
			const Steps* pSteps = GAMESTATE->m_pCurSteps[p];
			if( pSteps == NULL )
				return 0;
			bool bAlreadyAdded = find( vpStepsToShow.begin(), vpStepsToShow.end(), pSteps ) != vpStepsToShow.end();
			if( !bAlreadyAdded )
				vpStepsToShow.push_back( pSteps );
		}

		for( unsigned i=0; i<vpStepsToShow.size(); i++ )
		{
			const Steps* pSteps = vpStepsToShow[i];
			RString sDifficulty = DifficultyToLocalizedString( pSteps->GetDifficulty() );
			
			lua_pushstring( L, sDifficulty );
			lua_pushstring( L, pSteps->GetDescription() );
		}

		return vpStepsToShow.size()*2;
	}
	
	static int SetPreferredSongGroup( T* p, lua_State *L ) { p->m_sPreferredSongGroup.Set( SArg(1) ); return 0; }
	DEFINE_METHOD( GetPreferredSongGroup, m_sPreferredSongGroup.Get() );
	static int GetHumanPlayers( T* p, lua_State *L )
	{
		vector<PlayerNumber> vHP;
		FOREACH_HumanPlayer( pn )
			vHP.push_back( pn );
		LuaHelpers::CreateTableFromArray( vHP, L );
		return 1;
	}
	static int GetCurrentStyle( T* p, lua_State *L )
	{
		Style *pStyle = const_cast<Style *> (p->GetCurrentStyle());
		LuaHelpers::Push( L, pStyle );
		return 1;
	}
	static int IsAnyHumanPlayerUsingMemoryCard( T* p, lua_State *L )
	{
		bool bUsingMemoryCard = false;
		FOREACH_HumanPlayer( pn )
		{
			if( MEMCARDMAN->GetCardState(pn) == MemoryCardState_Ready )
				bUsingMemoryCard = true;
		}
		lua_pushboolean(L, bUsingMemoryCard );
		return 1;
	}
	static int GetNumStagesForCurrentSongAndStepsOrCourse( T* p, lua_State *L ) { lua_pushnumber(L, GAMESTATE->GetNumStagesForCurrentSongAndStepsOrCourse() ); return 1; }
	static int GetNumStagesLeft( T* p, lua_State *L )
	{
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		lua_pushnumber(L, p->GetNumStagesLeft(pn)); 
		return 1;
	}
	static int GetGameSeed( T* p, lua_State *L )			{ LuaHelpers::Push( L, p->m_iGameSeed ); return 1; }
	static int GetStageSeed( T* p, lua_State *L )			{ LuaHelpers::Push( L, p->m_iStageSeed ); return 1; }
	static int SaveLocalData( T* p, lua_State *L )			{ p->SaveLocalData(); return 0; }

	static int SetJukeboxUsesModifiers( T* p, lua_State *L )
	{
		p->m_bJukeboxUsesModifiers = BArg(1); return 0;
	}
	DEFINE_METHOD( GetWorkoutGoalComplete,		m_bWorkoutGoalComplete )

	LunaGameState() 
	{
		ADD_METHOD( IsPlayerEnabled );
		ADD_METHOD( IsHumanPlayer );
		ADD_METHOD( GetPlayerDisplayName );
		ADD_METHOD( GetMasterPlayerNumber );
		ADD_METHOD( GetMultiplayer );
		ADD_METHOD( GetNumMultiplayerNoteFields );
		ADD_METHOD( SetNumMultiplayerNoteFields );
		ADD_METHOD( GetPlayerState );
		ADD_METHOD( GetMultiPlayerState );
		ADD_METHOD( ApplyGameCommand );
		ADD_METHOD( GetCurrentSong );
		ADD_METHOD( SetCurrentSong );
		ADD_METHOD( GetCurrentSteps );
		ADD_METHOD( SetCurrentSteps );
		ADD_METHOD( GetCurrentCourse );
		ADD_METHOD( SetCurrentCourse );
		ADD_METHOD( GetCurrentTrail );
		ADD_METHOD( SetCurrentTrail );
		ADD_METHOD( SetPreferredSong );
		ADD_METHOD( GetPreferredSong );
		ADD_METHOD( SetTemporaryEventMode );
		ADD_METHOD( Env );
		ADD_METHOD( GetEditSourceSteps );
		ADD_METHOD( SetPreferredDifficulty );
		ADD_METHOD( GetPreferredDifficulty );
		ADD_METHOD( AnyPlayerHasRankingFeats );
		ADD_METHOD( IsCourseMode );
		ADD_METHOD( IsDemonstration );
		ADD_METHOD( GetPlayMode );
		ADD_METHOD( GetSortOrder );
		ADD_METHOD( GetStageIndex );
		ADD_METHOD( IsGoalComplete );
		ADD_METHOD( PlayerIsUsingModifier );
		ADD_METHOD( GetCourseSongIndex );
		ADD_METHOD( GetLoadingCourseSongIndex );
		ADD_METHOD( GetSmallestNumStagesLeftForAnyHumanPlayer );
		ADD_METHOD( IsAnExtraStage );
		ADD_METHOD( IsExtraStage );
		ADD_METHOD( IsExtraStage2 );
		ADD_METHOD( GetCurrentStage );
		ADD_METHOD( HasEarnedExtraStage );
		ADD_METHOD( GetEasiestStepsDifficulty );
		ADD_METHOD( IsEventMode );
		ADD_METHOD( GetNumPlayersEnabled );
		ADD_METHOD( GetSongBeat );
		ADD_METHOD( GetSongBeatVisible );
		ADD_METHOD( GetSongBPS );
		ADD_METHOD( GetSongFreeze );
		ADD_METHOD( GetGameplayLeadIn );
		ADD_METHOD( GetCoins );
		ADD_METHOD( IsSideJoined );
		ADD_METHOD( GetCoinsNeededToJoin );
		ADD_METHOD( EnoughCreditsToJoin );
		ADD_METHOD( PlayersCanJoin );
		ADD_METHOD( GetNumSidesJoined );
		ADD_METHOD( GetCoinMode );
		ADD_METHOD( GetPremium );
		ADD_METHOD( GetSongOptionsString );
		ADD_METHOD( GetSongOptions );
		ADD_METHOD( GetDefaultSongOptions );
		ADD_METHOD( SetSongOptions );
		ADD_METHOD( IsWinner );
		ADD_METHOD( IsDraw );
		ADD_METHOD( GetCurrentGame );
		ADD_METHOD( GetEditCourseEntryIndex );
		ADD_METHOD( GetEditLocalProfileID );
		ADD_METHOD( GetEditLocalProfile );
		ADD_METHOD( GetCurrentStepsCredits );
		ADD_METHOD( SetPreferredSongGroup );
		ADD_METHOD( GetPreferredSongGroup );
		ADD_METHOD( GetHumanPlayers );
		ADD_METHOD( GetCurrentStyle );
		ADD_METHOD( IsAnyHumanPlayerUsingMemoryCard );
		ADD_METHOD( GetNumStagesForCurrentSongAndStepsOrCourse );
		ADD_METHOD( GetNumStagesLeft );
		ADD_METHOD( GetGameSeed );
		ADD_METHOD( GetStageSeed );
		ADD_METHOD( SaveLocalData );
		ADD_METHOD( SetJukeboxUsesModifiers );
		ADD_METHOD( GetWorkoutGoalComplete );
	}
};

LUA_REGISTER_CLASS( GameState )
// lua end

/*
 * (c) 2001-2004 Chris Danford, Glenn Maynard, Chris Gomez
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
