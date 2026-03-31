package de.manhhao.hoshi

class HoshiDicts {
    companion object {
        init {
            System.loadLibrary("hoshidicts_jni")
        }
    }

    external fun createLookupObject(): Long
    
    external fun destroyLookupObject(session: Long)
    
    external fun rebuildQuery(
        session: Long, 
        termPaths: Array<String>, 
        freqPaths: Array<String>, 
        pitchPaths: Array<String>
    )
    
    external fun importDictionary(zipPath: String, outputDir: String): ImportResult
    
    external fun lookup(session: Long, text: String, maxResults: Int): Array<LookupResult>

    external fun queryExact(session: Long, expression: String): Array<TermResult>
    
    external fun getStyles(session: Long): Array<DictionaryStyle>
    
    external fun getMediaFile(session: Long, dictName: String, mediaPath: String): ByteArray?

    external fun hasMetaModeEntries(storagePath: String, mode: String, minCount: Int): Boolean

    fun isFrequencyDictionary(storagePath: String, minFreqEntryCount: Int = 5): Boolean {
        if (minFreqEntryCount <= 0) {
            return true
        }
        return hasMetaModeEntries(storagePath, "freq", minFreqEntryCount)
    }
}
