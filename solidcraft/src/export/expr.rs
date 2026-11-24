//! SolidCraft expressions -> NCX's neutral expression grammar (FORMAT-SPEC §4, Equations).
//!
//! SolidCraft writes `L/2 - 8`, `(12 mm) / 2`, `T + 15`, `30 deg`: parameter names bare, numbers with an optional
//! unit. NCX wants names in double quotes, lengths in mm and angles in degrees with no units, and only
//! `+ - * / ^ ( )`, `sin cos tan sqrt abs min max if`. Anything else is left out (the value still travels).

use std::collections::HashSet;

/// The expression in the neutral grammar, and whether it names a parameter (only those are worth an equation).
/// `None` when it uses something the grammar has no word for.
pub fn neutral(expr: &str, params: &HashSet<String>) -> Option<(String, bool)> {
    let chars: Vec<char> = expr.chars().collect();
    let mut out = String::new();
    let mut named = false;
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c.is_whitespace() {
            i += 1;
        } else if c.is_ascii_digit() || (c == '.' && chars.get(i + 1).map(|d| d.is_ascii_digit()).unwrap_or(false)) {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_digit() || chars[i] == '.') {
                i += 1;
            }
            // exponent: 1e-3, 2.5E4
            if i < chars.len() && (chars[i] == 'e' || chars[i] == 'E') {
                let mut j = i + 1;
                if j < chars.len() && (chars[j] == '+' || chars[j] == '-') {
                    j += 1;
                }
                if j < chars.len() && chars[j].is_ascii_digit() {
                    i = j;
                    while i < chars.len() && chars[i].is_ascii_digit() {
                        i += 1;
                    }
                }
            }
            let num: String = chars[start..i].iter().collect();
            // a unit right after the number (with or without a space)
            let mut j = i;
            while j < chars.len() && chars[j] == ' ' {
                j += 1;
            }
            let ustart = j;
            while j < chars.len() && (chars[j].is_alphabetic() || chars[j] == '°') {
                j += 1;
            }
            let unit: String = chars[ustart..j].iter().collect();
            match unit_factor(&unit) {
                Some(f) => {
                    i = j;
                    if (f - 1.0).abs() < 1e-15 {
                        out.push_str(&num);
                    } else {
                        out.push_str(&format!("({num} * {f})"));
                    }
                }
                None => out.push_str(&num), // not a unit: a name follows (`2 W` is not valid anyway)
            }
        } else if c.is_alphabetic() || c == '_' {
            let start = i;
            while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_') {
                i += 1;
            }
            let name: String = chars[start..i].iter().collect();
            if params.contains(&name) {
                out.push('"');
                out.push_str(&name);
                out.push('"');
                named = true;
            } else if ["sin", "cos", "tan", "sqrt", "abs", "min", "max", "if"].contains(&name.as_str()) {
                out.push_str(&name);
            } else if name == "pi" || name == "PI" {
                out.push_str("3.141592653589793");
            } else {
                return None;
            }
        } else if "+-*/^(),".contains(c) {
            out.push(c);
            if c != '(' && c != ')' {
                out.push(' ');
            }
            i += 1;
        } else {
            return None;
        }
    }
    let out = tidy(&out);
    if out.is_empty() {
        None
    } else {
        Some((out, named))
    }
}

/// mm (lengths) and degrees (angles) per unit; `None`: not a unit.
fn unit_factor(u: &str) -> Option<f64> {
    Some(match u {
        "" => 1.0,
        "mm" => 1.0,
        "cm" => 10.0,
        "m" => 1000.0,
        "in" | "inch" => 25.4,
        "ft" => 304.8,
        "deg" | "°" => 1.0,
        "rad" => 180.0 / std::f64::consts::PI,
        _ => return None,
    })
}

/// Spaces around binary operators, none after "(" or before ")".
fn tidy(s: &str) -> String {
    let mut out = String::new();
    let mut prev: Option<char> = None;
    for c in s.chars() {
        let binary = "+-*/^".contains(c) && prev.map(|p| p != '(' && p != ',' && !"+-*/^".contains(p)).unwrap_or(false);
        if binary && !out.ends_with(' ') {
            out.push(' ');
        }
        if c == ' ' && (out.ends_with(' ') || out.ends_with('(') || out.is_empty()) {
            continue;
        }
        if c == ')' && out.ends_with(' ') {
            out.pop();
        }
        out.push(c);
        if c != ' ' {
            prev = Some(c);
        }
    }
    out.trim().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    fn p() -> HashSet<String> {
        ["L", "W", "T"].iter().map(|s| s.to_string()).collect()
    }
    #[test]
    fn names_units_and_numbers() {
        assert_eq!(neutral("L/2 - 8", &p()), Some(("\"L\" / 2 - 8".into(), true)));
        assert_eq!(neutral("(12 mm) / 2", &p()), Some(("(12) / 2".into(), false)));
        assert_eq!(neutral("T + 15", &p()), Some(("\"T\" + 15".into(), true)));
        assert_eq!(neutral("1 in", &p()), Some(("(1 * 25.4)".into(), false)));
        assert_eq!(neutral("30 deg", &p()), Some(("30".into(), false)));
        assert_eq!(neutral("-W", &p()), Some(("-\"W\"".into(), true)));
        assert_eq!(neutral("Q + 1", &p()), None);
    }
}
