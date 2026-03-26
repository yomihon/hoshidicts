package de.manhhao.hoshi

data class ImportResult(
    val success: Boolean,
    val termCount: Long,
    val metaCount: Long,
    val mediaCount: Long
)

data class GlossaryEntry(
    val dictName: String,
    val glossary: String,
    val definitionTags: String,
    val termTags: String
)

data class Frequency(
    val value: Int,
    val displayValue: String
)

data class FrequencyEntry(
    val dictName: String,
    val frequencies: Array<Frequency>
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as FrequencyEntry

        if (dictName != other.dictName) return false
        if (!frequencies.contentEquals(other.frequencies)) return false

        return true
    }

    override fun hashCode(): Int {
        var result = dictName.hashCode()
        result = 31 * result + frequencies.contentHashCode()
        return result
    }
}

data class PitchEntry(
    val dictName: String,
    val pitchPositions: IntArray
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as PitchEntry

        if (dictName != other.dictName) return false
        if (!pitchPositions.contentEquals(other.pitchPositions)) return false

        return true
    }

    override fun hashCode(): Int {
        var result = dictName.hashCode()
        result = 31 * result + pitchPositions.contentHashCode()
        return result
    }
}

data class TermResult(
    val expression: String,
    val reading: String,
    val rules: String,
    val glossaries: Array<GlossaryEntry>,
    val frequencies: Array<FrequencyEntry>,
    val pitches: Array<PitchEntry>
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as TermResult

        if (expression != other.expression) return false
        if (reading != other.reading) return false
        if (rules != other.rules) return false
        if (!glossaries.contentEquals(other.glossaries)) return false
        if (!frequencies.contentEquals(other.frequencies)) return false
        if (!pitches.contentEquals(other.pitches)) return false

        return true
    }

    override fun hashCode(): Int {
        var result = expression.hashCode()
        result = 31 * result + reading.hashCode()
        result = 31 * result + rules.hashCode()
        result = 31 * result + glossaries.contentHashCode()
        result = 31 * result + frequencies.contentHashCode()
        result = 31 * result + pitches.contentHashCode()
        return result
    }
}

data class LookupResult(
    val matched: String,
    val deinflected: String,
    val process: Array<String>,
    val term: TermResult,
    val preprocessorSteps: Int
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false

        other as LookupResult

        if (matched != other.matched) return false
        if (deinflected != other.deinflected) return false
        if (!process.contentEquals(other.process)) return false
        if (term != other.term) return false
        if (preprocessorSteps != other.preprocessorSteps) return false

        return true
    }

    override fun hashCode(): Int {
        var result = matched.hashCode()
        result = 31 * result + deinflected.hashCode()
        result = 31 * result + process.contentHashCode()
        result = 31 * result + term.hashCode()
        result = 31 * result + preprocessorSteps
        return result
    }
}

data class DictionaryStyle(
    val dictName: String,
    val styles: String
)
